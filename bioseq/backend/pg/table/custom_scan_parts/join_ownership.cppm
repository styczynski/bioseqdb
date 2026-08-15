/**
 * @file
 * @brief Validates exact mmsearch ownership across join and wrapper path trees.
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

export module bioseq.postgres.table:custom_scan_join_ownership;

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

using namespace bioseq::postgres::support;

export {

    struct batch_join_target_projection_probe {
        Index target_varno = 0;
        AttrNumber target_seq_attno = InvalidAttrNumber;
        const bioseq_stub_oids* oids = nullptr;
        bool allow_mmsearch_qualifier = false;
        bool forbidden = false;
    };

    struct batch_join_target_projection_visitor {
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor,
                   pg_node_view node,
                   batch_join_target_projection_probe& probe) const noexcept {
            /* A validated mmsearch restriction consumes the locator inside the
             * BatchJoin executor, while match-result helpers are evaluated by its
             * parent from the current match_result. Neither argument is a request to
             * let the locator-backed datum escape into ordinary SQL evaluation. A
             * projected mmsearch call remains forbidden. */
            if (FuncExpr* function = node.try_cast<FuncExpr>()) {
                if (probe.oids != nullptr &&
                    (is_bioseq_projection_funcid(function->funcid, *probe.oids) ||
                     (probe.allow_mmsearch_qualifier &&
                      is_mmsearch_funcid(function->funcid, *probe.oids)))) {
                    return pg_walk_step::skip_subtree;
                }
            }
            if (Var* var = node.try_cast<Var>()) {
                if (var->varno == probe.target_varno &&
                    (var->varattno == 0 || var->varattno == probe.target_seq_attno)) {
                    probe.forbidden = true;
                    return pg_walk_step::stop;
                }
            }
            return pg_walk_step::descend;
        }
    };

    [[nodiscard]] bool batch_join_target_expression_is_forbidden(
        Node* expression, const Var* target_var, const bool allow_mmsearch_qualifier) {
        if (expression == nullptr || target_var == nullptr) {
            return false;
        }
        batch_join_target_projection_probe probe{target_var->varno,
                                                 target_var->varattno,
                                                 &get_cached_stub_oids(),
                                                 allow_mmsearch_qualifier,
                                                 false};
        batch_join_target_projection_visitor visitor;
        walk_pg_expression_including_root(expression, probe, visitor);
        return probe.forbidden;
    }

    [[nodiscard]] bool batch_join_target_restrictions_are_forbidden(
        List* restrictions, const Var* target_var, const bool allow_mmsearch_qualifier) {
        for (Node* node : postgres_list<Node>(restrictions)) {
            RestrictInfo* restriction = try_cast_pg_node<RestrictInfo>(node);
            if (restriction == nullptr || restriction->clause == nullptr ||
                batch_join_target_expression_is_forbidden(
                    reinterpret_cast<Node*>(restriction->clause),
                    target_var,
                    allow_mmsearch_qualifier)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool batch_join_typmod_is_supported(const int32 typmod) noexcept {
        if (typmod < 0) {
            return false;
        }
        const auto decoded = compact_sequence_stdc_type_params::decode(typmod);
        const bool alphabet_supported =
            decoded.alphabet_code < pgseq_supported_alphabets::alphabet_count;
        const bool width_supported = pgseq_supported_storage_unit_byte_widths::contains_runtime(
            decoded.storage_unit_byte_width);
        return alphabet_supported && width_supported && decoded.encode() == typmod;
    }

    [[noreturn]] void
    raise_mmsearch_alphabet_mismatch(const compact_sequence_stdc_type_params target,
                                     const compact_sequence_stdc_type_params query,
                                     const char* stage) {
        const auto target_name =
            pgseq_supported_alphabets::name_of_alphabet_code(target.alphabet_code);
        const auto query_name =
            pgseq_supported_alphabets::name_of_alphabet_code(query.alphabet_code);
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("bioseq mmsearch requires operands with the same bound alphabet"),
                 errdetail("stage=%s target=seq('%.*s', %u) query=seq('%.*s', %u)",
                           stage,
                           static_cast<int>(target_name.size()),
                           target_name.data(),
                           static_cast<unsigned>(target.storage_unit_byte_width) * 8u,
                           static_cast<int>(query_name.size()),
                           query_name.data(),
                           static_cast<unsigned>(query.storage_unit_byte_width) * 8u),
                 errhint("Explicitly convert the data before binding it to one alphabet; mmsearch "
                         "never performs cross-alphabet recoding.")));
        pg_unreachable();
    }

    [[nodiscard]] bool mmsearch_typmods_share_alphabet(const int32 target_typmod,
                                                       const int32 query_typmod) noexcept {
        return batch_join_typmod_is_supported(target_typmod) &&
               batch_join_typmod_is_supported(query_typmod) &&
               compact_sequence_stdc_type_params::decode(target_typmod).alphabet_code ==
                   compact_sequence_stdc_type_params::decode(query_typmod).alphabet_code;
    }

    struct per_row_sequence_typmod_validation {
        int32 typmod = -1;
        bool descriptor_erased = false;
    };

    [[nodiscard]] bool per_row_function_signature_is_exact(FuncExpr* function) {
        if (function == nullptr || function->funcretset || OidIsValid(function->funcvariadic))
            return false;
        Oid* catalog_argument_types = nullptr;
        int catalog_argument_count = 0;
        const Oid catalog_result_type =
            get_func_signature(function->funcid, &catalog_argument_types, &catalog_argument_count);
        if (catalog_result_type != function->funcresulttype ||
            catalog_argument_count != list_length(function->args))
            return false;
        int argument_index = 0;
        for (Node* argument : postgres_list<Node>(function->args)) {
            if (argument == nullptr ||
                exprType(argument) != catalog_argument_types[argument_index++])
                return false;
        }
        return true;
    }

    [[nodiscard]] bool per_row_operator_signature_is_exact(OpExpr* operation) {
        if (operation == nullptr || !OidIsValid(operation->opno) ||
            !OidIsValid(operation->opfuncid) || operation->opretset ||
            get_opcode(operation->opno) != operation->opfuncid)
            return false;
        Oid* catalog_argument_types = nullptr;
        int catalog_argument_count = 0;
        const Oid catalog_result_type = get_func_signature(
            operation->opfuncid, &catalog_argument_types, &catalog_argument_count);
        if (catalog_result_type != operation->opresulttype ||
            catalog_argument_count != list_length(operation->args))
            return false;
        int argument_index = 0;
        for (Node* argument : postgres_list<Node>(operation->args)) {
            if (argument == nullptr ||
                exprType(argument) != catalog_argument_types[argument_index++])
                return false;
        }
        return true;
    }

    /* PostgreSQL does not propagate typmods through ordinary FuncExpr results,
     * even when the exact catalog function is sequence-type preserving.  Admit an
     * erased descriptor only after recursively validating one identical concrete
     * typmod at every sequence-valued leaf.  Non-sequence literal/array operands
     * are neutral only for the exact registered concat overloads. */
    [[nodiscard]] std::optional<per_row_sequence_typmod_validation>
    per_row_validate_sequence_typmod(Node* expression, bioseq_stub_oids const& oids) {
        check_stack_depth();
        if (expression == nullptr || exprType(expression) != oids.bioseq_seq_type_oid) {
            return std::nullopt;
        }
        const int32 descriptor_typmod = exprTypmod(expression);
        auto finish =
            [descriptor_typmod](
                const int32 validated_typmod) -> std::optional<per_row_sequence_typmod_validation> {
            if (!batch_join_typmod_is_supported(validated_typmod) ||
                (descriptor_typmod != -1 && descriptor_typmod != validated_typmod)) {
                return std::nullopt;
            }
            return per_row_sequence_typmod_validation{.typmod = validated_typmod,
                                                      .descriptor_erased = descriptor_typmod == -1};
        };
        if (IsA(expression, RelabelType)) {
            auto* relabel = reinterpret_cast<RelabelType*>(expression);
            if (relabel->resulttype != oids.bioseq_seq_type_oid || relabel->arg == nullptr)
                return std::nullopt;
            auto child =
                per_row_validate_sequence_typmod(reinterpret_cast<Node*>(relabel->arg), oids);
            return child.has_value() ? finish(child->typmod) : std::nullopt;
        }
        const auto operation = slice_project_node_opcode(expression, oids);
        if (!operation.has_value()) {
            return batch_join_typmod_is_supported(descriptor_typmod) ? finish(descriptor_typmod)
                                                                     : std::nullopt;
        }
        if (*operation == slice_project_opcode::slice) {
            auto* subscript = reinterpret_cast<SubscriptingRef*>(expression);
            if (subscript->refcontainertype != oids.bioseq_seq_type_oid ||
                subscript->refrestype != oids.bioseq_seq_type_oid ||
                subscript->reflowerindexpr == NIL || subscript->refexpr == nullptr)
                return std::nullopt;
            auto source =
                per_row_validate_sequence_typmod(reinterpret_cast<Node*>(subscript->refexpr), oids);
            return source.has_value() ? finish(source->typmod) : std::nullopt;
        }
        if (*operation == slice_project_opcode::reverse ||
            *operation == slice_project_opcode::reverse_complement) {
            FuncExpr* function = try_cast_pg_node<FuncExpr>(expression);
            if (function == nullptr || function->funcresulttype != oids.bioseq_seq_type_oid ||
                !per_row_function_signature_is_exact(function)) {
                return std::nullopt;
            }
            if (list_length(function->args) != 1)
                return std::nullopt;
            auto source = per_row_validate_sequence_typmod(
                reinterpret_cast<Node*>(linitial(function->args)), oids);
            return source.has_value() ? finish(source->typmod) : std::nullopt;
        }
        if (*operation != slice_project_opcode::concat)
            return std::nullopt;
        List* arguments = NIL;
        if (FuncExpr* function = try_cast_pg_node<FuncExpr>(expression)) {
            if (function->funcresulttype != oids.bioseq_seq_type_oid ||
                !slice_project_concat_oid(function->funcid, oids) ||
                !per_row_function_signature_is_exact(function)) {
                return std::nullopt;
            }
            arguments = function->args;
        } else if (OpExpr* operator_expression = try_cast_pg_node<OpExpr>(expression)) {
            if (operator_expression->opresulttype != oids.bioseq_seq_type_oid ||
                !slice_project_concat_oid(operator_expression->opfuncid, oids) ||
                list_length(operator_expression->args) != 2 ||
                !per_row_operator_signature_is_exact(operator_expression)) {
                return std::nullopt;
            }
            arguments = operator_expression->args;
        } else {
            return std::nullopt;
        }
        std::optional<int32> common_typmod;
        for (Node* argument : postgres_list<Node>(arguments)) {
            if (exprType(argument) != oids.bioseq_seq_type_oid)
                continue;
            auto child = per_row_validate_sequence_typmod(argument, oids);
            if (!child.has_value() ||
                (common_typmod.has_value() && *common_typmod != child->typmod))
                return std::nullopt;
            common_typmod = child->typmod;
        }
        return common_typmod.has_value() ? finish(*common_typmod) : std::nullopt;
    }

    /* Reject a Gather hidden below the common unary planner wrappers.  Looking
     * only at cheapest_total_path->pathtype misses Projection/Material wrappers
     * and lets a parallel child enter an executor that owns one backend-local
     * query materialization. */
    [[nodiscard]] bool batch_join_path_contains_parallel_execution(const Path* path) noexcept {
        if (path == nullptr) {
            return false;
        }
        if (path->pathtype == T_Gather || path->pathtype == T_GatherMerge || path->parallel_aware ||
            path->parallel_workers > 0) {
            return true;
        }
        switch (path->pathtype) {
        case T_Result:
            return batch_join_path_contains_parallel_execution(
                reinterpret_cast<const ProjectionPath*>(path)->subpath);
        case T_Material:
            return batch_join_path_contains_parallel_execution(
                reinterpret_cast<const MaterialPath*>(path)->subpath);
        case T_Memoize:
            return batch_join_path_contains_parallel_execution(
                reinterpret_cast<const MemoizePath*>(path)->subpath);
        case T_Sort:
            return batch_join_path_contains_parallel_execution(
                reinterpret_cast<const SortPath*>(path)->subpath);
        case T_IncrementalSort:
            return batch_join_path_contains_parallel_execution(
                reinterpret_cast<const IncrementalSortPath*>(path)->spath.subpath);
        case T_NestLoop:
        case T_MergeJoin:
        case T_HashJoin: {
            auto const* join = reinterpret_cast<JoinPath const*>(path);
            return batch_join_path_contains_parallel_execution(join->outerjoinpath) ||
                   batch_join_path_contains_parallel_execution(join->innerjoinpath);
        }
        case T_CustomScan: {
            auto const* custom = reinterpret_cast<CustomPath const*>(path);
            for (Path* child : postgres_list<Path>(custom->custom_paths)) {
                if (batch_join_path_contains_parallel_execution(child)) {
                    return true;
                }
            }
            return false;
        }
        case T_Append: {
            auto const* append = reinterpret_cast<AppendPath const*>(path);
            for (Path* child : postgres_list<Path>(append->subpaths)) {
                if (batch_join_path_contains_parallel_execution(child)) {
                    return true;
                }
            }
            return false;
        }
        case T_MergeAppend: {
            auto const* append = reinterpret_cast<MergeAppendPath const*>(path);
            for (Path* child : postgres_list<Path>(append->subpaths)) {
                if (batch_join_path_contains_parallel_execution(child)) {
                    return true;
                }
            }
            return false;
        }
        default:
            return false;
        }
    }

    struct batch_join_bioseq_path_candidate {
        CustomPath* scan = nullptr;
        Path* cost_owner = nullptr;
        const PathTarget* locator_target = nullptr;
    };

    [[nodiscard]] bool batch_join_expression_has_match_helper(Node* expression,
                                                              const bioseq_stub_oids& oids);

    [[nodiscard]] bool path_target_has_equal_expression(const PathTarget* target,
                                                        const Node* expression) noexcept {
        if (target == nullptr || expression == nullptr) {
            return false;
        }
        for (Node* existing : postgres_list<Node>(target->exprs)) {
            if (equal(existing, expression)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] FuncExpr* batch_join_exact_target_helper(Node* expression,
                                                           const Index target_relid) noexcept {
        FuncExpr* helper = try_cast_pg_node<FuncExpr>(expression);
        if (!bioseq_is_concrete_rte_varno(target_relid) || helper == nullptr ||
            !is_bioseq_projection_funcid(helper->funcid, get_cached_stub_oids()) ||
            helper->funcretset || OidIsValid(helper->funcvariadic) ||
            get_func_rettype(helper->funcid) != helper->funcresulttype ||
            list_length(helper->args) != 1) {
            return nullptr;
        }
        Var* argument = try_cast_pg_node<Var>(reinterpret_cast<Node*>(linitial(helper->args)));
        return argument != nullptr && argument->varno == target_relid && argument->varattno > 0 &&
                       argument->varlevelsup == 0 && bms_is_empty(argument->varnullingrels)
                   ? helper
                   : nullptr;
    }

    /* Decode only the catalog/arity/type contract of a match-result helper.  Its
     * argument binding is resolved separately against the selected owner recipe
     * after standard_planner has assigned final plan_node_id values. */
    [[nodiscard]] FuncExpr*
    batch_join_well_formed_match_helper(Node* expression, const bioseq_stub_oids& oids) noexcept {
        FuncExpr* helper = try_cast_pg_node<FuncExpr>(expression);
        if (helper == nullptr || !is_bioseq_projection_funcid(helper->funcid, oids) ||
            helper->funcretset || OidIsValid(helper->funcvariadic) ||
            get_func_rettype(helper->funcid) != helper->funcresulttype ||
            list_length(helper->args) != 1) {
            return nullptr;
        }
        return helper;
    }

    [[nodiscard]] Node* batch_join_match_helper_argument(FuncExpr* helper) noexcept {
        return helper == nullptr || list_length(helper->args) != 1
                   ? nullptr
                   : reinterpret_cast<Node*>(linitial(helper->args));
    }

    [[nodiscard]] List* canonical_match_base_lineage(Node* target_recipe,
                                                     const bioseq_stub_oids& oids);

    [[nodiscard]] List* make_match_owner_descriptor(Node* target_recipe,
                                                    Index base_relid,
                                                    const bioseq_stub_oids& oids);

    [[nodiscard]] bool batch_join_match_helper_names_recipe(FuncExpr* helper,
                                                            Node* target_recipe) noexcept {
        Node* argument = batch_join_match_helper_argument(helper);
        return argument != nullptr && target_recipe != nullptr && equal(argument, target_recipe);
    }

    [[nodiscard]] bool batch_join_match_helper_names_base_lineage(FuncExpr* helper,
                                                                  Node* target_recipe,
                                                                  const bioseq_stub_oids& oids) {
        Var* argument = try_cast_pg_node<Var>(batch_join_match_helper_argument(helper));
        if (argument == nullptr || target_recipe == nullptr ||
            !bioseq_is_concrete_rte_varno(argument->varno) || argument->varattno <= 0 ||
            argument->varlevelsup != 0 || !bms_is_empty(argument->varnullingrels))
            return false;
        for (Var* base : postgres_list<Var>(canonical_match_base_lineage(target_recipe, oids))) {
            if (base == nullptr)
                continue;
            if (argument->vartype == oids.bioseq_seq_type_oid) {
                if (equal(argument, base))
                    return true;
            } else if (argument->varno == base->varno &&
                       argument->varlevelsup == base->varlevelsup) {
                return true;
            }
        }
        return false;
    }

    /* Generic PHV shape used by transformed exact-recipe helpers.  Unlike the
     * base-lineage helper decoder below, the PHV may cover more than one relation when
     * the target recipe is a concat.  The final owner resolver validates the
     * canonical argument and lineage before any executor sees the site. */
    [[nodiscard]] FuncExpr*
    batch_join_match_helper_phv_function(const PlaceHolderVar* placeholder,
                                         const bioseq_stub_oids& oids) noexcept {
        if (placeholder == nullptr || placeholder->phid == 0 || placeholder->phlevelsup != 0 ||
            !bms_is_empty(placeholder->phnullingrels) || bms_is_empty(placeholder->phrels)) {
            return nullptr;
        }
        return batch_join_well_formed_match_helper(reinterpret_cast<Node*>(placeholder->phexpr),
                                                   oids);
    }

    /* Exact shape emitted by try_wrap_in_placeholder() for a direct helper call.
     * Do not accept a generic PHV merely because a helper occurs somewhere below
     * it: its evaluation level/nulling semantics may belong to unrelated SQL. */
    [[nodiscard]] FuncExpr*
    batch_join_exact_helper_phv_function(const PlaceHolderVar* placeholder,
                                         const Index target_relid) noexcept {
        if (placeholder == nullptr || !bioseq_is_concrete_rte_varno(target_relid) ||
            placeholder->phid == 0 || placeholder->phlevelsup != 0 ||
            !bms_is_empty(placeholder->phnullingrels) ||
            bms_num_members(placeholder->phrels) != 1 ||
            !bms_is_member(static_cast<int>(target_relid), placeholder->phrels)) {
            return nullptr;
        }
        return batch_join_exact_target_helper(reinterpret_cast<Node*>(placeholder->phexpr),
                                              target_relid);
    }

    [[nodiscard]] bool batch_join_is_exact_helper_phv(const PlaceHolderVar* placeholder,
                                                      const Index target_relid) noexcept {
        return batch_join_exact_helper_phv_function(placeholder, target_relid) != nullptr;
    }

    struct batch_join_projection_delta {
        const PathTarget* child_target = nullptr;
        Index target_relid = 0;
        bool saw_exact_helper_phv = false;
        bool invalid = false;
    };

    struct batch_join_projection_delta_visitor {
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor,
                   pg_node_view node,
                   batch_join_projection_delta& validation) const noexcept {
            if (PlaceHolderVar* placeholder = node.try_cast<PlaceHolderVar>()) {
                if (!batch_join_is_exact_helper_phv(placeholder, validation.target_relid)) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
                validation.saw_exact_helper_phv = true;
                return pg_walk_step::skip_subtree;
            }
            if (Var* var = node.try_cast<Var>()) {
                if (!path_target_has_equal_expression(validation.child_target,
                                                      reinterpret_cast<Node*>(var))) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
            }
            return pg_walk_step::descend;
        }
    };

    /* Validate that the complete wrapper-only expression can be evaluated by the
     * BatchJoin parent without changing semantics: it is nonvolatile, every Var
     * is already produced by the child, every PHV is the exact helper-support
     * shape, and at least one such helper PHV explains the wrapper delta. */
    [[nodiscard]] bool batch_join_projection_delta_is_parent_safe(
        Node* expression, const PathTarget* child_target, const Index target_relid) noexcept {
        if (expression == nullptr || child_target == nullptr ||
            contain_volatile_functions(expression)) {
            return false;
        }
        batch_join_projection_delta validation{child_target, target_relid, false, false};
        batch_join_projection_delta_visitor visitor;
        walk_pg_expression_including_root(expression, validation, visitor);
        return !validation.invalid && validation.saw_exact_helper_phv;
    }

    /* A wrapper may also expose an ordinary target attribute needed above the
     * join (for example t.pk).  Carrying that base Var through the locator child
     * is exact; carrying the sequence datum, a whole row, a system attribute, a
     * nulled outer-join Var, or an expression assembled at this level is not. */
    [[nodiscard]] bool batch_join_is_safe_plain_target_var(
        Node* expression, const Index target_relid, const AttrNumber target_seq_attno) noexcept {
        Var* var = try_cast_pg_node<Var>(expression);
        return var != nullptr && var->varno == target_relid && var->varattno > 0 &&
               var->varattno != target_seq_attno && var->varlevelsup == 0 &&
               bms_is_empty(var->varnullingrels);
    }

    /* A helper support function can add a ProjectionPath at the base relation.
     * BioseqBatchJoin may look through it only when every wrapper-only expression
     * is either an exact helper expression evaluated by the parent or one plain,
     * non-sequence target Var copied into the locator child. Any other expression
     * is a real evaluation boundary and disqualifies this candidate. */
    [[nodiscard]] bool
    batch_join_projection_is_parent_safe(const ProjectionPath* projection,
                                         const AttrNumber target_seq_attno) noexcept {
        if (projection == nullptr || projection->subpath == nullptr ||
            projection->path.pathtype != T_Result ||
            projection->path.parent != projection->subpath->parent ||
            projection->path.parent == nullptr ||
            projection->path.parent->reloptkind != RELOPT_BASEREL ||
            projection->path.parent->relid == 0 || projection->path.param_info != nullptr ||
            projection->subpath->param_info != nullptr ||
            !equal(projection->path.pathkeys, projection->subpath->pathkeys) ||
            projection->path.pathtarget == nullptr || projection->subpath->pathtarget == nullptr) {
            return false;
        }
        bool saw_safe_delta = false;
        for (Node* expression : postgres_list<Node>(projection->path.pathtarget->exprs)) {
            if (path_target_has_equal_expression(projection->subpath->pathtarget, expression)) {
                continue;
            }
            if (batch_join_is_safe_plain_target_var(
                    expression, projection->path.parent->relid, target_seq_attno)) {
                saw_safe_delta = true;
                continue;
            }
            if (!batch_join_projection_delta_is_parent_safe(
                    expression, projection->subpath->pathtarget, projection->path.parent->relid)) {
                return false;
            }
            saw_safe_delta = true;
        }
        return saw_safe_delta;
    }

    [[nodiscard]] batch_join_bioseq_path_candidate
    find_unparameterized_bioseq_custom_path(RelOptInfo* rel,
                                            const AttrNumber target_seq_attno) noexcept {
        batch_join_bioseq_path_candidate best{};
        for (Path* path : postgres_list<Path>(rel->pathlist)) {
            if (path == nullptr || path->param_info != nullptr) {
                continue;
            }
            Path* cost_owner = path;
            const PathTarget* locator_target = path->pathtarget;
            /* Planner support can expose safe base attributes and wraps match-result helpers in
             * PlaceHolderVars at their originating base relation.  PostgreSQL can
             * consequently expose the otherwise eligible BioseqScan as the child
             * of a projection path.  The projection must not run in the locator
             * child: BioseqBatchJoin rewrites those helpers to parent-owned result
             * placeholders after the match_result exists.  Look through exactly
             * that validated parent-safe wrapper and retain the underlying scan. */
            if (IsA(path, ProjectionPath)) {
                auto* projection = reinterpret_cast<ProjectionPath*>(path);
                if (!batch_join_projection_is_parent_safe(projection, target_seq_attno) ||
                    batch_join_path_contains_parallel_execution(&projection->path)) {
                    continue;
                }
                locator_target = projection->path.pathtarget;
                path = projection->subpath;
            }
            if (path == nullptr || path->pathtype != T_CustomScan || path->param_info != nullptr ||
                batch_join_path_contains_parallel_execution(path)) {
                continue;
            }
            auto* custom = reinterpret_cast<CustomPath*>(path);
            if (custom->methods != bioseq_column_scan_path_methods()) {
                continue;
            }
            if (best.scan == nullptr ||
                compare_path_costs(cost_owner, best.cost_owner, TOTAL_COST) < 0) {
                best = batch_join_bioseq_path_candidate{custom, cost_owner, locator_target};
            }
        }
        return best;
    }

    /*
     * Integration-only validation seam for the exact production eligibility guard.
     * SQL cannot reliably force an unrelated expression into a base-relation
     * ProjectionPath: PostgreSQL will normally retain it in an upper projection.
     * Construct the dangerous planner shapes directly and pass them through
     * find_unparameterized_bioseq_custom_path(), which is the production entry
     * point used by BioseqBatchJoinSetJoinPathlistHook().
     */
    struct batch_join_projection_guard_fixture {
        RelOptInfo* rel = nullptr;
        CustomPath* scan = nullptr;
        ProjectionPath* projection = nullptr;
    };

    [[nodiscard]] PlaceHolderVar* make_batch_join_projection_guard_phv(const Oid helper_oid,
                                                                       const Oid result_type,
                                                                       const Index relid,
                                                                       const Index phid) {
        auto* argument = makeVar(relid, 1, INT4OID, -1, InvalidOid, 0);
        auto* helper = makeFuncExpr(helper_oid,
                                    result_type,
                                    list_make1(argument),
                                    InvalidOid,
                                    InvalidOid,
                                    COERCE_EXPLICIT_CALL);
        auto* placeholder = makeNode(PlaceHolderVar);
        placeholder->phexpr = reinterpret_cast<Expr*>(helper);
        placeholder->phrels = bms_make_singleton(static_cast<int>(relid));
        placeholder->phnullingrels = nullptr;
        placeholder->phid = phid;
        placeholder->phlevelsup = 0;
        return placeholder;
    }

    [[nodiscard]] batch_join_projection_guard_fixture
    make_batch_join_projection_guard_fixture(const Index relid,
                                             List* wrapper_delta,
                                             const Cost wrapper_startup,
                                             const Cost wrapper_total,
                                             const Cardinality wrapper_rows) {
        auto* rel = makeNode(RelOptInfo);
        rel->reloptkind = RELOPT_BASEREL;
        rel->relid = relid;

        auto* child_target = create_empty_pathtarget();
        add_column_to_pathtarget(
            child_target,
            reinterpret_cast<Expr*>(makeVar(relid, 1, INT4OID, -1, InvalidOid, 0)),
            0);
        auto* wrapper_target = copy_pathtarget(child_target);
        for (Node* expression : postgres_list<Node>(wrapper_delta)) {
            add_column_to_pathtarget(wrapper_target, reinterpret_cast<Expr*>(expression), 0);
        }

        auto* scan = makeNode(CustomPath);
        scan->path.pathtype = T_CustomScan;
        scan->path.parent = rel;
        scan->path.pathtarget = child_target;
        scan->path.param_info = nullptr;
        scan->path.parallel_aware = false;
        scan->path.parallel_safe = false;
        scan->path.parallel_workers = 0;
        scan->path.rows = wrapper_rows;
        scan->path.startup_cost = 1.0;
        scan->path.total_cost = 2.0;
        scan->path.pathkeys = NIL;
        scan->methods = bioseq_column_scan_path_methods();

        auto* projection = makeNode(ProjectionPath);
        projection->path.pathtype = T_Result;
        projection->path.parent = rel;
        projection->path.pathtarget = wrapper_target;
        projection->path.param_info = nullptr;
        projection->path.parallel_aware = false;
        projection->path.parallel_safe = false;
        projection->path.parallel_workers = 0;
        projection->path.rows = wrapper_rows;
        projection->path.startup_cost = wrapper_startup;
        projection->path.total_cost = wrapper_total;
        projection->path.pathkeys = NIL;
        projection->subpath = &scan->path;
        projection->dummypp = false;
        rel->pathlist = list_make1(projection);
        return batch_join_projection_guard_fixture{rel, scan, projection};
    }

    [[nodiscard]] bool path_target_contains_expression(const PathTarget* target,
                                                       const Expr* expression) noexcept {
        if (target == nullptr || expression == nullptr) {
            return false;
        }
        for (Node* existing : postgres_list<Node>(target->exprs)) {
            if (equal(existing, expression)) {
                return true;
            }
        }
        return false;
    }

    struct batch_join_underlying_target_var_collector {
        PathTarget* destination = nullptr;
        Index target_relid = 0;
        AttrNumber target_seq_attno = InvalidAttrNumber;
        bool invalid = false;
    };

    struct batch_join_underlying_target_var_visitor {
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor,
                   pg_node_view node,
                   batch_join_underlying_target_var_collector& collector) const {
            Var* var = node.try_cast<Var>();
            if (var == nullptr) {
                return pg_walk_step::descend;
            }
            if (var->varno != collector.target_relid) {
                return pg_walk_step::skip_subtree;
            }
            const bool exact_sequence_var = var->varattno == collector.target_seq_attno &&
                                            var->varattno > 0 && var->varlevelsup == 0 &&
                                            bms_is_empty(var->varnullingrels);
            if (!exact_sequence_var &&
                !batch_join_is_safe_plain_target_var(reinterpret_cast<Node*>(var),
                                                     collector.target_relid,
                                                     collector.target_seq_attno)) {
                collector.invalid = true;
                return pg_walk_step::stop;
            }
            /* The sequence Var is installed explicitly below.  Preserve every other
             * safe target Var, including a helper's load-bearing argument (normally
             * t.pk) hidden below a PlaceHolderVar/compound wrapper.  Skipping the
             * whole wrapper because it contains a bioseq helper must not erase that
             * base-column dependency from the locator child and combined scan tlist. */
            if (!exact_sequence_var && collector.destination != nullptr &&
                !path_target_contains_expression(collector.destination,
                                                 reinterpret_cast<Expr*>(var))) {
                add_column_to_pathtarget(
                    collector.destination, reinterpret_cast<Expr*>(copyObject(var)), 0);
            }
            return pg_walk_step::skip_subtree;
        }
    };

    [[nodiscard]] bool batch_join_append_underlying_target_vars(PathTarget* destination,
                                                                const PathTarget* source,
                                                                const Index target_relid,
                                                                const AttrNumber target_seq_attno) {
        if (destination == nullptr || source == nullptr ||
            !bioseq_is_concrete_rte_varno(target_relid) || target_seq_attno <= 0) {
            return false;
        }
        batch_join_underlying_target_var_collector collector{
            destination, target_relid, target_seq_attno, false};
        batch_join_underlying_target_var_visitor visitor;
        for (Node* expression : postgres_list<Node>(source->exprs)) {
            if (expression == nullptr) {
                continue;
            }
            walk_pg_expression_including_root(expression, collector, visitor);
            if (collector.invalid) {
                return false;
            }
        }
        return true;
    }

    int next_batch_join_plan_token = 1;

    [[nodiscard]] Const* make_batch_join_cost_const(const Cost value) {
        if (!std::isfinite(value)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin planner cost is not finite")));
        }
        return makeConst(FLOAT8OID,
                         -1,
                         InvalidOid,
                         static_cast<int16>(sizeof(float8)),
                         Float8GetDatum(value),
                         false,
                         FLOAT8PASSBYVAL);
    }

    [[nodiscard]] Cost batch_join_cost_from_node(Node* node, const char* field) {
        Const* value = try_cast_pg_node<Const>(node);
        if (value == nullptr || value->consttype != FLOAT8OID || value->constisnull ||
            value->constlen != static_cast<int16>(sizeof(float8)) ||
            value->constbyval != FLOAT8PASSBYVAL) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin cost ABI is invalid"),
                     errdetail("field=%s", field)));
        }
        const Cost decoded = DatumGetFloat8(value->constvalue);
        if (!std::isfinite(decoded)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin cost ABI is not finite"),
                     errdetail("field=%s", field)));
        }
        return decoded;
    }

    struct per_row_operand_source_resolution {
        RelOptInfo* joinrel = nullptr;
        List* sequence_vars = NIL;
        bool invalid = false;
    };

    struct per_row_operand_source_visitor {
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              per_row_operand_source_resolution& resolution) const {
            if (node.try_cast<PlaceHolderVar>() != nullptr) {
                /* Moving an operand PHV to the post-join recipe can change its
                 * evaluation/nulling frontier.  No sequence helper requires that
                 * representation, so decline instead of approximating semantics. */
                resolution.invalid = true;
                return pg_walk_step::stop;
            }
            Var* var = node.try_cast<Var>();
            if (var == nullptr)
                return pg_walk_step::descend;
            if (resolution.joinrel == nullptr || !bioseq_is_concrete_rte_varno(var->varno) ||
                var->varattno <= 0 || var->varlevelsup != 0 || !bms_is_empty(var->varnullingrels) ||
                !bms_is_member(static_cast<int>(var->varno), resolution.joinrel->relids)) {
                resolution.invalid = true;
                return pg_walk_step::stop;
            }
            if (var->vartype == get_cached_stub_oids().bioseq_seq_type_oid) {
                bool seen = false;
                for (Node* existing : postgres_list<Node>(resolution.sequence_vars)) {
                    if (equal(existing, var)) {
                        seen = true;
                        break;
                    }
                }
                if (!seen)
                    resolution.sequence_vars = lappend(resolution.sequence_vars, var);
            }
            return pg_walk_step::skip_subtree;
        }
    };

    struct per_row_output_semantic_validation {
        Index target_relid = 0;
        Relids join_relids = nullptr;
        Node* target_recipe = nullptr;
        bool invalid = false;
    };

    [[nodiscard]] bool per_row_is_exact_row_identity_var(const Var* var) noexcept {
        if (var == nullptr || !bioseq_is_concrete_rte_varno(var->varno) || var->varlevelsup != 0 ||
            !bms_is_empty(var->varnullingrels))
            return false;
        return (var->varattno == SelfItemPointerAttributeNumber && var->vartype == TIDOID &&
                var->vartypmod == -1 && !OidIsValid(var->varcollid)) ||
               (var->varattno == TableOidAttributeNumber && var->vartype == OIDOID &&
                var->vartypmod == -1 && !OidIsValid(var->varcollid));
    }

    struct per_row_output_semantic_visitor {
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor,
                   pg_node_view node,
                   per_row_output_semantic_validation& validation) const {
            if (PlaceHolderVar* placeholder = node.try_cast<PlaceHolderVar>()) {
                /* The exact helper PHV is deliberately evaluated by the BatchJoin
                 * owner after match_result production.  At an intermediate joinrel the
                 * reltarget can also carry a whole helper PHV for a transformed owner
                 * that will be constructed at a higher join frontier.  Admit that exact
                 * sequence-valued identity here; the higher-owner COW normalization
                 * removes it from every descendant target before PlanCustomPath.  Every
                 * generic PHV retains a planner-selected frontier which this
                 * transformation cannot move. */
                FuncExpr* helper =
                    batch_join_match_helper_phv_function(placeholder, get_cached_stub_oids());
                Node* argument = batch_join_match_helper_argument(helper);
                const bool possible_higher_owner =
                    helper != nullptr && argument != nullptr &&
                    exprType(argument) == get_cached_stub_oids().bioseq_seq_type_oid;
                if (helper == nullptr ||
                    (!batch_join_match_helper_names_recipe(helper, validation.target_recipe) &&
                     !batch_join_match_helper_names_base_lineage(
                         helper, validation.target_recipe, get_cached_stub_oids()) &&
                     !possible_higher_owner)) {
                    validation.invalid = true;
                }
                return validation.invalid ? pg_walk_step::stop : pg_walk_step::skip_subtree;
            }
            if (Var* var = node.try_cast<Var>()) {
                /* UPDATE ... FROM EPQ carries exact ctid/tableoid row markers for
                 * non-target join inputs as well as the target.  They are ordinary
                 * inner-join passthrough values, not arbitrary system attributes. */
                if (per_row_is_exact_row_identity_var(var) &&
                    bms_is_member(static_cast<int>(var->varno), validation.join_relids)) {
                    return pg_walk_step::skip_subtree;
                }
                if (!bioseq_is_concrete_rte_varno(var->varno) || var->varattno <= 0 ||
                    var->varlevelsup != 0 || !bms_is_empty(var->varnullingrels)) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
                return pg_walk_step::skip_subtree;
            }
            return pg_walk_step::descend;
        }
    };

    [[nodiscard]] bool per_row_output_semantics_are_supported(PathTarget* target,
                                                              const Index target_relid,
                                                              Relids join_relids,
                                                              Node* target_recipe) {
        if (target == nullptr || !bioseq_is_concrete_rte_varno(target_relid) ||
            join_relids == nullptr || target_recipe == nullptr) {
            return false;
        }
        /* Volatile final-output expressions stay in the owner PathTarget and are
         * evaluated once by core ProjectionInfo for each emitted match.  This
         * validation governs only nodes that would otherwise lose a planner frontier. */
        per_row_output_semantic_validation validation{
            target_relid, join_relids, target_recipe, false};
        per_row_output_semantic_visitor visitor;
        for (Node* expression : postgres_list<Node>(target->exprs)) {
            walk_pg_expression_including_root(expression, validation, visitor);
            if (validation.invalid)
                return false;
        }
        return true;
    }

    struct per_row_owner_support_validation {
        Index target_relid = 0;
        Relids join_relids = nullptr;
        const bioseq_stub_oids* oids = nullptr;
        Node* target_recipe = nullptr;
        List* passthrough_vars = NIL;
        List* helper_identities = NIL;
        /* Sequence-valued helpers which do not name this owner may still be exact
         * outputs of one versioned descendant BatchJoin.  They remain candidates
         * until each incumbent path validates that ownership structurally. */
        List* foreign_exact_helper_identities = NIL;
        List* helper_argument_vars = NIL;
        bool invalid = false;
    };

    void per_row_append_unique_expression(List*& expressions, Node* node) {
        if (node == nullptr)
            return;
        for (Node* existing : postgres_list<Node>(expressions)) {
            if (equal(existing, node))
                return;
        }
        expressions = lappend(expressions, copyObject(node));
    }

    [[nodiscard]] bool per_row_expression_list_contains(List* expressions, Node* node) noexcept {
        if (node == nullptr)
            return false;
        for (Node* existing : postgres_list<Node>(expressions)) {
            if (existing != nullptr && equal(existing, node))
                return true;
        }
        return false;
    }

    [[nodiscard]] bool per_row_authoritative_sortgrouprefs(PlannerInfo* root,
                                                           List* expressions,
                                                           std::vector<Index>& results);

    /* Keep the public CustomPath target exactly at PostgreSQL's authoritative
     * joinrel frontier.  Planner-only helper and passthrough columns are appended
     * by PlanCustomPath after core has finished applying scan/join targets.  This
     * avoids exposing a widened expr list to apply_scanjoin_target_to_paths(),
     * which can otherwise install the join target's shorter sortgroupref array. */
    [[nodiscard]] PathTarget* per_row_build_owner_target(PlannerInfo* root, PathTarget* original) {
        if (root == nullptr || original == nullptr)
            return nullptr;
        std::vector<Index> sortgrouprefs;
        if (!per_row_authoritative_sortgrouprefs(root, original->exprs, sortgrouprefs))
            return nullptr;
        if (sortgrouprefs.size() != static_cast<std::size_t>(list_length(original->exprs)))
            return nullptr;
        PathTarget* rebuilt = create_empty_pathtarget();
        rebuilt->exprs = static_cast<List*>(copyObject(original->exprs));
        const bool has_sortgroupref = std::any_of(sortgrouprefs.begin(),
                                                  sortgrouprefs.end(),
                                                  [](Index value) noexcept { return value != 0; });
        if (has_sortgroupref) {
            rebuilt->sortgrouprefs =
                static_cast<Index*>(palloc0(sortgrouprefs.size() * sizeof(Index)));
            for (std::size_t index = 0u; index < sortgrouprefs.size(); ++index) {
                rebuilt->sortgrouprefs[index] = sortgrouprefs[index];
            }
        }
        Assert(list_length(rebuilt->exprs) == list_length(original->exprs));
        return rebuilt;
    }

    /* Core's upper setrefs pass can descend a plain match-helper FuncExpr before
     * finalized match_result binding.  It then needs the raw Vars below the exact
     * transformed target recipe, plus valid base-lineage helper arguments.
     * These are Plan-only resjunk carriers; they must not widen the visible owner
     * PathTarget. */
    [[nodiscard]] bool per_row_collect_setrefs_carrier_vars(Node* target_recipe,
                                                            List* helper_argument_vars,
                                                            Relids join_relids,
                                                            List*& carrier_vars) {
        carrier_vars = NIL;
        if (target_recipe == nullptr || join_relids == nullptr)
            return false;
        constexpr int pull_flags =
            PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
        List* recipe_vars = pull_var_clause(target_recipe, pull_flags);
        auto append_valid = [join_relids, &carrier_vars](Node* node) -> bool {
            Var* var = try_cast_pg_node<Var>(node);
            const bool row_identity = per_row_is_exact_row_identity_var(var);
            if (var == nullptr || !bioseq_is_concrete_rte_varno(var->varno) ||
                (var->varattno <= 0 && !row_identity) || var->varlevelsup != 0 ||
                !bms_is_empty(var->varnullingrels) ||
                !bms_is_member(static_cast<int>(var->varno), join_relids))
                return false;
            per_row_append_unique_expression(carrier_vars, node);
            return true;
        };
        for (Node* recipe_var : postgres_list<Node>(recipe_vars)) {
            if (!append_valid(recipe_var))
                return false;
        }
        for (Node* helper_argument : postgres_list<Node>(helper_argument_vars)) {
            if (!append_valid(helper_argument))
                return false;
        }
        return carrier_vars != NIL;
    }

    /* Core can defer the SELECT/RETURNING projection until above the join and,
     * for a projection-capable CustomPath, overwrite plan.targetlist only after
     * PlanCustomPath has returned.  Collect the narrow support surface up front:
     * level-zero join Vars are passed through, while exact target-owned match
     * helpers are carried as whole descriptor identities.  Their validated single
     * target Var is collected separately for reconstructed scan-leaf and Plan-only
     * resjunk setrefs edges; it is never substituted for the final whole-helper
     * output. */
    struct per_row_owner_support_visitor {
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              per_row_owner_support_validation& validation) const {
            if (PlaceHolderVar* placeholder = node.try_cast<PlaceHolderVar>()) {
                FuncExpr* helper =
                    validation.oids == nullptr
                        ? nullptr
                        : batch_join_match_helper_phv_function(placeholder, *validation.oids);
                if (helper != nullptr) {
                    const bool names_recipe =
                        batch_join_match_helper_names_recipe(helper, validation.target_recipe);
                    const bool names_lineage = batch_join_match_helper_names_base_lineage(
                        helper, validation.target_recipe, *validation.oids);
                    Node* argument = batch_join_match_helper_argument(helper);
                    const bool possible_foreign_exact =
                        !names_recipe && !names_lineage && argument != nullptr &&
                        exprType(argument) == validation.oids->bioseq_seq_type_oid;
                    if (!names_recipe && !names_lineage && !possible_foreign_exact) {
                        validation.invalid = true;
                        return pg_walk_step::stop;
                    }
                    /* PostgreSQL can spell the same helper site as its whole PHV in
                     * the join reltarget and as the underlying FuncExpr in an upper
                     * Sort/Result target.  Preserve both exact identities so setrefs
                     * can bind either spelling to a descriptor-only match_result site;
                     * never descend into the helper argument Var. */
                    per_row_append_unique_expression(validation.helper_identities,
                                                     reinterpret_cast<Node*>(placeholder));
                    per_row_append_unique_expression(validation.helper_identities,
                                                     reinterpret_cast<Node*>(helper));
                    if (possible_foreign_exact) {
                        per_row_append_unique_expression(validation.foreign_exact_helper_identities,
                                                         reinterpret_cast<Node*>(placeholder));
                        per_row_append_unique_expression(validation.foreign_exact_helper_identities,
                                                         reinterpret_cast<Node*>(helper));
                    }
                    /* Core may retain a Result directly above the target BioseqScan
                     * while carrying the whole PHV to the final owner.  Preserve the
                     * exact single target-owned argument at that scan edge so setrefs
                     * can bind Result's support Var without executing the helper. */
                    if (names_lineage) {
                        per_row_append_unique_expression(validation.helper_argument_vars,
                                                         batch_join_match_helper_argument(helper));
                    }
                    return pg_walk_step::skip_subtree;
                }
                if (validation.oids != nullptr &&
                    batch_join_expression_has_match_helper(
                        reinterpret_cast<Node*>(placeholder->phexpr), *validation.oids)) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
                /* A generic PHV owns its planner-selected evaluation frontier. */
                return pg_walk_step::skip_subtree;
            }
            if (FuncExpr* function = node.try_cast<FuncExpr>();
                validation.oids != nullptr && function != nullptr &&
                is_bioseq_projection_funcid(function->funcid, *validation.oids)) {
                const bool names_recipe =
                    batch_join_match_helper_names_recipe(function, validation.target_recipe);
                const bool base_lineage = batch_join_match_helper_names_base_lineage(
                    function, validation.target_recipe, *validation.oids);
                Node* argument = batch_join_match_helper_argument(function);
                const bool possible_foreign_exact =
                    !names_recipe && !base_lineage && argument != nullptr &&
                    exprType(argument) == validation.oids->bioseq_seq_type_oid;
                if (!names_recipe && !base_lineage && !possible_foreign_exact) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
                per_row_append_unique_expression(validation.helper_identities,
                                                 reinterpret_cast<Node*>(function));
                if (possible_foreign_exact) {
                    per_row_append_unique_expression(validation.foreign_exact_helper_identities,
                                                     reinterpret_cast<Node*>(function));
                }
                if (base_lineage) {
                    per_row_append_unique_expression(validation.helper_argument_vars,
                                                     batch_join_match_helper_argument(function));
                }
                return pg_walk_step::skip_subtree;
            }
            if (Var* var = node.try_cast<Var>()) {
                if (var->varlevelsup != 0) {
                    return pg_walk_step::skip_subtree;
                }
                if (!bioseq_is_concrete_rte_varno(var->varno)) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
                if (!bms_is_member(static_cast<int>(var->varno), validation.join_relids)) {
                    return pg_walk_step::skip_subtree;
                }
                const bool row_identity = per_row_is_exact_row_identity_var(var);
                if ((var->varattno <= 0 && !row_identity) || !bms_is_empty(var->varnullingrels)) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
                per_row_append_unique_expression(validation.passthrough_vars,
                                                 reinterpret_cast<Node*>(var));
                return pg_walk_step::skip_subtree;
            }
            return pg_walk_step::descend;
        }
    };

    [[nodiscard]] bool per_row_collect_owner_support(PlannerInfo* root,
                                                     RelOptInfo* joinrel,
                                                     const Index target_relid,
                                                     Node* target_recipe,
                                                     per_row_owner_support_validation& validation) {
        if (root == nullptr || joinrel == nullptr || !bioseq_is_concrete_rte_varno(target_relid) ||
            joinrel->relids == nullptr || target_recipe == nullptr) {
            return false;
        }
        validation.target_relid = target_relid;
        validation.join_relids = joinrel->relids;
        validation.oids = &get_cached_stub_oids();
        validation.target_recipe = target_recipe;
        per_row_owner_support_visitor visitor;
        for (TargetEntry* output : postgres_list<TargetEntry>(root->processed_tlist)) {
            if (output == nullptr || output->expr == nullptr)
                continue;
            walk_pg_expression_including_root(
                reinterpret_cast<Node*>(output->expr), validation, visitor);
            if (validation.invalid)
                return false;
        }
        return true;
    }

    [[nodiscard]] bool per_row_collect_operand_sources(
        Node* expression, RelOptInfo* joinrel, per_row_operand_source_resolution& resolution) {
        if (expression == nullptr || joinrel == nullptr || contain_volatile_functions(expression))
            return false;
        resolution.joinrel = joinrel;
        per_row_operand_source_visitor visitor;
        walk_pg_expression_including_root(expression, resolution, visitor);
        return !resolution.invalid;
    }

    /* Exact baserel operand validation for BioseqScan-owned mmsearch.  Unlike the
     * join relocation validation above, this expression is not moved across a planner
     * frontier: its original post-setrefs ExprState remains authoritative at the
     * scan.  Volatile functions and opaque/erroring CASE/COALESCE arms are thus
     * legal and execute once in PostgreSQL order.  The remaining validation
     * requires that no aggregate/window/subplan/PHV/SRF frontier is crossed and that every
     * Var belongs to this non-nullable scan relation. */
    struct base_local_mmsearch_operand_resolution {
        Node* expression = nullptr;
        List* sequence_vars = NIL;
        Var* primary_sequence_var = nullptr;
        int32 typmod = -1;
        bool evaluated = false;
    };

    struct base_local_mmsearch_operand_walker {
        RelOptInfo* rel = nullptr;
        const bioseq_stub_oids* oids = nullptr;
        List* sequence_vars = NIL;
        bool invalid = false;
    };

    struct base_local_mmsearch_operand_visitor {
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor,
                   pg_node_view node,
                   base_local_mmsearch_operand_walker& validation_state) const {
            Node* raw = node.get();
            if (raw == nullptr || validation_state.rel == nullptr ||
                validation_state.oids == nullptr) {
                validation_state.invalid = true;
                return pg_walk_step::stop;
            }
            if (IsA(raw, PlaceHolderVar) || IsA(raw, Aggref) || IsA(raw, GroupingFunc) ||
                IsA(raw, WindowFunc) || IsA(raw, SubPlan) || IsA(raw, AlternativeSubPlan) ||
                IsA(raw, SubLink)) {
                validation_state.invalid = true;
                return pg_walk_step::stop;
            }
            if (FuncExpr* function = node.try_cast<FuncExpr>()) {
                if (function->funcretset ||
                    is_mmsearch_funcid(function->funcid, *validation_state.oids)) {
                    validation_state.invalid = true;
                    return pg_walk_step::stop;
                }
            }
            if (OpExpr* operation = node.try_cast<OpExpr>();
                operation != nullptr && operation->opretset) {
                validation_state.invalid = true;
                return pg_walk_step::stop;
            }
            Var* variable = node.try_cast<Var>();
            if (variable == nullptr)
                return pg_walk_step::descend;
            if (!bioseq_is_concrete_rte_varno(variable->varno) ||
                variable->varno != validation_state.rel->relid || variable->varattno <= 0 ||
                variable->varlevelsup != 0 || !bms_is_empty(variable->varnullingrels)) {
                validation_state.invalid = true;
                return pg_walk_step::stop;
            }
            if (variable->vartype == validation_state.oids->bioseq_seq_type_oid) {
                bool duplicate = false;
                for (Var* existing : postgres_list<Var>(validation_state.sequence_vars)) {
                    if (equal(existing, variable)) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate)
                    validation_state.sequence_vars =
                        lappend(validation_state.sequence_vars, variable);
            }
            return pg_walk_step::skip_subtree;
        }
    };

    struct base_local_sequence_typmod_resolution {
        int32 typmod = -1;
        bool invalid = false;
    };

    [[nodiscard]] base_local_sequence_typmod_resolution
    resolve_base_local_sequence_typmod(Node* expression, const bioseq_stub_oids& oids) {
        check_stack_depth();
        if (expression == nullptr || exprType(expression) != oids.bioseq_seq_type_oid) {
            return {.invalid = true};
        }
        auto supported_descriptor = [](const int32 typmod) noexcept {
            return typmod >= 0 && batch_join_typmod_is_supported(typmod);
        };
        auto finish = [expression,
                       &supported_descriptor](base_local_sequence_typmod_resolution derived) {
            if (derived.invalid)
                return derived;
            const int32 descriptor = exprTypmod(expression);
            if (descriptor < -1 || (descriptor >= 0 && !supported_descriptor(descriptor))) {
                return base_local_sequence_typmod_resolution{.invalid = true};
            }
            if (descriptor >= 0) {
                if (derived.typmod >= 0) {
                    const auto declared = compact_sequence_stdc_type_params::decode(descriptor);
                    const auto child = compact_sequence_stdc_type_params::decode(derived.typmod);
                    if (declared.alphabet_code != child.alphabet_code) {
                        return base_local_sequence_typmod_resolution{.invalid = true};
                    }
                }
                derived.typmod = descriptor;
            }
            return derived;
        };
        auto merge_exact = [](base_local_sequence_typmod_resolution& destination,
                              const base_local_sequence_typmod_resolution source) {
            if (source.invalid) {
                destination.invalid = true;
                return;
            }
            /* An opaque, descriptor-erased arm is intentionally neutral.  If it is
             * selected, capture_target/query_datum validates the actual bound type
             * before dispatch; an unselected throwing arm is never inspected. */
            if (source.typmod < 0)
                return;
            if (destination.typmod < 0) {
                destination.typmod = source.typmod;
                return;
            }
            if (destination.typmod != source.typmod)
                destination.invalid = true;
        };

        if (Node* source = slice_project_typmod_cast_source(expression, oids)) {
            base_local_sequence_typmod_resolution child =
                resolve_base_local_sequence_typmod(source, oids);
            Const* target_typmod = try_cast_pg_node<Const>(
                reinterpret_cast<Node*>(lsecond(reinterpret_cast<FuncExpr*>(expression)->args)));
            if (child.invalid || target_typmod == nullptr || target_typmod->constisnull)
                return {.invalid = true};
            const int32 cast_typmod = DatumGetInt32(target_typmod->constvalue);
            if (!supported_descriptor(cast_typmod))
                return {.invalid = true};
            if (child.typmod >= 0 &&
                compact_sequence_stdc_type_params::decode(child.typmod).alphabet_code !=
                    compact_sequence_stdc_type_params::decode(cast_typmod).alphabet_code)
                return {.invalid = true};
            return base_local_sequence_typmod_resolution{cast_typmod, false};
        }
        if (IsA(expression, RelabelType)) {
            auto* relabel = reinterpret_cast<RelabelType*>(expression);
            return finish(
                resolve_base_local_sequence_typmod(reinterpret_cast<Node*>(relabel->arg), oids));
        }
        if (nodeTag(expression) == T_CaseExpr) {
            CaseExpr* choice = castNode(CaseExpr, expression);
            base_local_sequence_typmod_resolution result{};
            for (CaseWhen* arm : postgres_list<CaseWhen>(choice->args)) {
                if (arm == nullptr || arm->result == nullptr ||
                    exprType(reinterpret_cast<Node*>(arm->result)) != oids.bioseq_seq_type_oid) {
                    result.invalid = true;
                    break;
                }
                merge_exact(
                    result,
                    resolve_base_local_sequence_typmod(reinterpret_cast<Node*>(arm->result), oids));
                if (result.invalid)
                    break;
            }
            if (!result.invalid && choice->defresult != nullptr) {
                merge_exact(result,
                            resolve_base_local_sequence_typmod(
                                reinterpret_cast<Node*>(choice->defresult), oids));
            }
            return finish(result);
        }
        if (nodeTag(expression) == T_CoalesceExpr) {
            CoalesceExpr* choice = castNode(CoalesceExpr, expression);
            base_local_sequence_typmod_resolution result{};
            for (Node* arm : postgres_list<Node>(choice->args)) {
                merge_exact(result, resolve_base_local_sequence_typmod(arm, oids));
                if (result.invalid)
                    break;
            }
            return finish(result);
        }

        const auto opcode = slice_project_node_opcode(expression, oids);
        if (opcode.has_value() && *opcode == slice_project_opcode::slice) {
            auto* subscript = reinterpret_cast<SubscriptingRef*>(expression);
            if (subscript->refexpr == nullptr || subscript->refassgnexpr != nullptr ||
                subscript->refcontainertype != oids.bioseq_seq_type_oid ||
                subscript->refrestype != oids.bioseq_seq_type_oid) {
                return {.invalid = true};
            }
            return finish(resolve_base_local_sequence_typmod(
                reinterpret_cast<Node*>(subscript->refexpr), oids));
        }
        if (opcode.has_value() && (*opcode == slice_project_opcode::reverse ||
                                   *opcode == slice_project_opcode::reverse_complement)) {
            FuncExpr* function = try_cast_pg_node<FuncExpr>(expression);
            if (function == nullptr || list_length(function->args) != 1 ||
                !per_row_function_signature_is_exact(function)) {
                return {.invalid = true};
            }
            return finish(resolve_base_local_sequence_typmod(
                reinterpret_cast<Node*>(linitial(function->args)), oids));
        }
        if (opcode.has_value() && *opcode == slice_project_opcode::concat) {
            List* arguments = NIL;
            if (FuncExpr* function = try_cast_pg_node<FuncExpr>(expression)) {
                if (!slice_project_concat_oid(function->funcid, oids) ||
                    !per_row_function_signature_is_exact(function)) {
                    return {.invalid = true};
                }
                arguments = function->args;
            } else if (OpExpr* operation = try_cast_pg_node<OpExpr>(expression)) {
                if (!slice_project_concat_oid(operation->opfuncid, oids) ||
                    !per_row_operator_signature_is_exact(operation)) {
                    return {.invalid = true};
                }
                arguments = operation->args;
            } else {
                return {.invalid = true};
            }
            base_local_sequence_typmod_resolution result{};
            for (Node* argument : postgres_list<Node>(arguments)) {
                if (exprType(argument) != oids.bioseq_seq_type_oid)
                    continue;
                const auto child = resolve_base_local_sequence_typmod(argument, oids);
                if (child.invalid)
                    return child;
                if (child.typmod < 0)
                    continue;
                if (result.typmod < 0) {
                    /* Registered concat uses the first bound sequence operand's
                     * storage width; later bound operands need only share alphabet. */
                    result.typmod = child.typmod;
                    continue;
                }
                if (compact_sequence_stdc_type_params::decode(result.typmod).alphabet_code !=
                    compact_sequence_stdc_type_params::decode(child.typmod).alphabet_code)
                    return {.invalid = true};
            }
            return finish(result);
        }

        /* Raw Vars/Consts/Params and opaque sequence-producing arms are legal
         * leaves.  A concrete descriptor identifies their dispatch type.  An erased
         * opaque leaf is neutral only when an enclosing CASE/COALESCE supplies a
         * concrete sibling; it cannot be the root of an owned operand. */
        const int32 descriptor = exprTypmod(expression);
        return descriptor >= 0 && supported_descriptor(descriptor)
                   ? base_local_sequence_typmod_resolution{descriptor, false}
                   : base_local_sequence_typmod_resolution{};
    }

    [[nodiscard]] std::optional<base_local_mmsearch_operand_resolution>
    resolve_base_local_mmsearch_operand(Node* expression,
                                        RelOptInfo* rel,
                                        const bioseq_stub_oids& oids,
                                        const bool require_sequence_var) {
        if (expression == nullptr || rel == nullptr ||
            exprType(expression) != oids.bioseq_seq_type_oid ||
            expression_returns_set(expression) || contain_subplans(expression))
            return std::nullopt;
        base_local_mmsearch_operand_walker walker{rel, &oids, NIL, false};
        base_local_mmsearch_operand_visitor visitor;
        walk_pg_expression_including_root(expression, walker, visitor);
        if (walker.invalid || (require_sequence_var && walker.sequence_vars == NIL)) {
            return std::nullopt;
        }
        const base_local_sequence_typmod_resolution type =
            resolve_base_local_sequence_typmod(expression, oids);
        if (type.invalid || type.typmod < 0 || !batch_join_typmod_is_supported(type.typmod))
            return std::nullopt;
        Var* raw = try_cast_pg_node<Var>(expression);
        return base_local_mmsearch_operand_resolution{
            .expression = expression,
            .sequence_vars = walker.sequence_vars,
            .primary_sequence_var = walker.sequence_vars == NIL
                                        ? nullptr
                                        : reinterpret_cast<Var*>(linitial(walker.sequence_vars)),
            .typmod = type.typmod,
            .evaluated = raw == nullptr,
        };
    }

    struct base_local_mmsearch_ownership {
        base_local_mmsearch_operand_resolution target{};
        std::optional<base_local_mmsearch_operand_resolution> local_query{};
        bool query_is_const = false;
        bool query_is_param = false;
        bool query_is_outer_var = false;
        bool any_evaluated_operand = false;
    };

    [[nodiscard]] std::optional<base_local_mmsearch_ownership>
    resolve_base_local_mmsearch_ownership(FuncExpr* function,
                                          RelOptInfo* rel,
                                          Relids param_relids,
                                          const bioseq_stub_oids& oids) {
        if (function == nullptr || rel == nullptr || !is_mmsearch_funcid(function->funcid, oids))
            return std::nullopt;
        pg_first_two_node_arguments arguments =
            get_pg_first_two_arguments_if_arity_between(*function, 2, 3);
        if (!arguments.is_present())
            return std::nullopt;
        auto target = resolve_base_local_mmsearch_operand(arguments.first_argument,
                                                          rel,
                                                          oids,
                                                          /*require_sequence_var=*/true);
        if (!target.has_value())
            return std::nullopt;
        base_local_mmsearch_ownership result{};
        result.target = *target;
        result.any_evaluated_operand = target->evaluated;
        Node* query = arguments.second_argument;
        if (Const* constant = try_cast_pg_node<Const>(query)) {
            if (constant->consttype != oids.bioseq_seq_type_oid || constant->consttypmod < 0 ||
                !batch_join_typmod_is_supported(constant->consttypmod) ||
                (!constant->constisnull &&
                 !mmsearch_typmods_share_alphabet(target->typmod, constant->consttypmod))) {
                return std::nullopt;
            }
            result.query_is_const = true;
            return result;
        }
        if (Param* parameter = try_cast_pg_node<Param>(query)) {
            const bool runtime_typed_external =
                parameter->paramkind == PARAM_EXTERN && parameter->paramtypmod < 0;
            if (parameter->paramtype != oids.bioseq_seq_type_oid ||
                (parameter->paramkind != PARAM_EXEC && parameter->paramkind != PARAM_EXTERN) ||
                (!runtime_typed_external &&
                 (parameter->paramtypmod < 0 ||
                  !batch_join_typmod_is_supported(parameter->paramtypmod) ||
                  !mmsearch_typmods_share_alphabet(target->typmod, parameter->paramtypmod)))) {
                return std::nullopt;
            }
            result.query_is_param = true;
            return result;
        }
        if (Var* variable = try_cast_pg_node<Var>(query);
            variable != nullptr && variable->varno != rel->relid) {
            if (!bioseq_is_concrete_rte_varno(variable->varno) ||
                !bms_is_member(static_cast<int>(variable->varno), param_relids) ||
                variable->varattno <= 0 || variable->varlevelsup != 0 ||
                !bms_is_empty(variable->varnullingrels) ||
                variable->vartype != oids.bioseq_seq_type_oid || variable->vartypmod < 0 ||
                !batch_join_typmod_is_supported(variable->vartypmod) ||
                !mmsearch_typmods_share_alphabet(target->typmod, variable->vartypmod))
                return std::nullopt;
            result.query_is_outer_var = true;
            return result;
        }
        auto local_query =
            resolve_base_local_mmsearch_operand(query, rel, oids, /*require_sequence_var=*/false);
        if (!local_query.has_value() ||
            !mmsearch_typmods_share_alphabet(target->typmod, local_query->typmod))
            return std::nullopt;
        result.any_evaluated_operand = result.any_evaluated_operand || local_query->evaluated;
        result.local_query = *local_query;
        return result;
    }

    [[nodiscard]] List* per_row_without_restriction(List* restrictions, RestrictInfo* owned) {
        List* filtered = NIL;
        for (RestrictInfo* candidate : postgres_list<RestrictInfo>(restrictions)) {
            if (candidate != owned)
                filtered = lappend(filtered, candidate);
        }
        return filtered;
    }

    /* A RestrictInfo can migrate below a NestLoop through PathParamInfo (or sit
     * in another custom/join child) while still appearing in the incumbent's
     * joinrestrictinfo.  Removing only the top copy would leave a second live
     * mmsearch rendezvous.  The post-join owner therefore accepts an incumbent
     * only when the exact pointer is absent from every retained descendant. */
    [[nodiscard]] bool per_row_descendant_owns_restriction(Path const* path,
                                                           RestrictInfo const* owned) noexcept {
        if (path == nullptr || owned == nullptr)
            return false;
        if (path->param_info != nullptr && list_member_ptr(path->param_info->ppi_clauses, owned))
            return true;
        if (IsA(path, CustomPath)) {
            auto const* custom = reinterpret_cast<CustomPath const*>(path);
            if (list_member_ptr(custom->custom_restrictinfo, owned))
                return true;
            for (Path* child : postgres_list<Path>(custom->custom_paths)) {
                if (per_row_descendant_owns_restriction(child, owned))
                    return true;
            }
            return false;
        }
        if (IsA(path, NestPath) || IsA(path, MergePath) || IsA(path, HashPath)) {
            auto const* join = reinterpret_cast<JoinPath const*>(path);
            if (list_member_ptr(join->joinrestrictinfo, owned))
                return true;
            return per_row_descendant_owns_restriction(join->outerjoinpath, owned) ||
                   per_row_descendant_owns_restriction(join->innerjoinpath, owned);
        }
        switch (path->pathtype) {
        case T_Result:
            return per_row_descendant_owns_restriction(
                reinterpret_cast<ProjectionPath const*>(path)->subpath, owned);
        case T_Material:
            return per_row_descendant_owns_restriction(
                reinterpret_cast<MaterialPath const*>(path)->subpath, owned);
        case T_Memoize:
            return per_row_descendant_owns_restriction(
                reinterpret_cast<MemoizePath const*>(path)->subpath, owned);
        case T_Sort:
            return per_row_descendant_owns_restriction(
                reinterpret_cast<SortPath const*>(path)->subpath, owned);
        case T_IncrementalSort:
            return per_row_descendant_owns_restriction(
                reinterpret_cast<IncrementalSortPath const*>(path)->spath.subpath, owned);
        case T_Append: {
            auto const* append = reinterpret_cast<AppendPath const*>(path);
            for (Path* child : postgres_list<Path>(append->subpaths)) {
                if (per_row_descendant_owns_restriction(child, owned))
                    return true;
            }
            return false;
        }
        case T_MergeAppend: {
            auto const* append = reinterpret_cast<MergeAppendPath const*>(path);
            for (Path* child : postgres_list<Path>(append->subpaths)) {
                if (per_row_descendant_owns_restriction(child, owned))
                    return true;
            }
            return false;
        }
        default:
            return false;
        }
    }

    struct per_row_mmsearch_plan_audit {
        std::size_t executable_qual_copies = 0u;
        std::size_t vector_qual_copies = 0u;
        std::size_t storage_qual_copies = 0u;
        std::size_t descendant_owner_copies = 0u;

        [[nodiscard]] bool clean() const noexcept {
            return executable_qual_copies == 0u && vector_qual_copies == 0u &&
                   storage_qual_copies == 0u && descendant_owner_copies == 0u;
        }
    };

    void per_row_audit_mmsearch_expression(Node* expression,
                                           std::size_t& destination,
                                           bioseq_stub_oids const& oids) {
        if (expression == nullptr)
            return;
        const std::size_t additional = count_mmsearch_occurrences(expression, oids);
        destination = additional > std::numeric_limits<std::size_t>::max() - destination
                          ? std::numeric_limits<std::size_t>::max()
                          : destination + additional;
    }

    struct exact_mmsearch_owner_audit {
        FuncExpr* candidate = nullptr;
        bioseq_stub_oids const* oids = nullptr;
        std::size_t matches = 0u;
    };

    struct exact_mmsearch_owner_audit_visitor {
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              exact_mmsearch_owner_audit& audit) const {
            FuncExpr* function = node.try_cast<FuncExpr>();
            if (function == nullptr || audit.oids == nullptr ||
                !is_mmsearch_funcid(function->funcid, *audit.oids)) {
                return pg_walk_step::descend;
            }
            if (audit.candidate != nullptr && equal(function, audit.candidate)) {
                audit.matches = audit.matches == std::numeric_limits<std::size_t>::max()
                                    ? audit.matches
                                    : audit.matches + 1u;
            }
            return pg_walk_step::skip_subtree;
        }
    };

    void per_row_audit_exact_mmsearch_owner(Node* expression,
                                            FuncExpr* candidate,
                                            std::size_t& destination,
                                            bioseq_stub_oids const& oids) {
        if (expression == nullptr || candidate == nullptr)
            return;
        exact_mmsearch_owner_audit audit{candidate, &oids, 0u};
        exact_mmsearch_owner_audit_visitor visitor;
        walk_pg_expression_including_root(expression, audit, visitor);
        destination = audit.matches > std::numeric_limits<std::size_t>::max() - destination
                          ? std::numeric_limits<std::size_t>::max()
                          : destination + audit.matches;
    }

    /* Audit the serialized child plan that will execute below the transformed
     * owner.  `Plan::qual` is only one of PostgreSQL's executable-qual stores:
     * join/index/recheck fields and the Bioseq reader's storage/vector lists must
     * be inspected independently.  The fourth BioseqScan expression list is an
     * executor-only mmsearch owner.  A foreign owner is valid nested SQL state;
     * only an owner equal to the candidate recipe is a duplicate of this owner. */
    void per_row_audit_mmsearch_plan(Plan* plan,
                                     per_row_mmsearch_plan_audit& audit,
                                     bioseq_stub_oids const& oids,
                                     FuncExpr* exact_owner_candidate = nullptr) {
        if (plan == nullptr)
            return;
        per_row_audit_mmsearch_expression(
            reinterpret_cast<Node*>(plan->qual), audit.executable_qual_copies, oids);

        if (IsA(plan, NestLoop)) {
            auto* join = &reinterpret_cast<NestLoop*>(plan)->join;
            per_row_audit_mmsearch_expression(
                reinterpret_cast<Node*>(join->joinqual), audit.executable_qual_copies, oids);
        } else if (IsA(plan, MergeJoin)) {
            auto* merge = reinterpret_cast<MergeJoin*>(plan);
            per_row_audit_mmsearch_expression(
                reinterpret_cast<Node*>(merge->join.joinqual), audit.executable_qual_copies, oids);
            per_row_audit_mmsearch_expression(
                reinterpret_cast<Node*>(merge->mergeclauses), audit.executable_qual_copies, oids);
        } else if (IsA(plan, HashJoin)) {
            auto* hash = reinterpret_cast<HashJoin*>(plan);
            per_row_audit_mmsearch_expression(
                reinterpret_cast<Node*>(hash->join.joinqual), audit.executable_qual_copies, oids);
            per_row_audit_mmsearch_expression(
                reinterpret_cast<Node*>(hash->hashclauses), audit.executable_qual_copies, oids);
        } else if (IsA(plan, Result)) {
            per_row_audit_mmsearch_expression(reinterpret_cast<Result*>(plan)->resconstantqual,
                                              audit.executable_qual_copies,
                                              oids);
        } else if (IsA(plan, IndexScan)) {
            auto* scan = reinterpret_cast<IndexScan*>(plan);
            for (List* expressions : {scan->indexqual,
                                      scan->indexqualorig,
                                      scan->indexorderby,
                                      scan->indexorderbyorig}) {
                per_row_audit_mmsearch_expression(
                    reinterpret_cast<Node*>(expressions), audit.executable_qual_copies, oids);
            }
        } else if (IsA(plan, IndexOnlyScan)) {
            auto* scan = reinterpret_cast<IndexOnlyScan*>(plan);
            for (List* expressions : {scan->indexqual, scan->recheckqual, scan->indexorderby}) {
                per_row_audit_mmsearch_expression(
                    reinterpret_cast<Node*>(expressions), audit.executable_qual_copies, oids);
            }
        } else if (IsA(plan, BitmapIndexScan)) {
            auto* scan = reinterpret_cast<BitmapIndexScan*>(plan);
            for (List* expressions : {scan->indexqual, scan->indexqualorig}) {
                per_row_audit_mmsearch_expression(
                    reinterpret_cast<Node*>(expressions), audit.executable_qual_copies, oids);
            }
        } else if (IsA(plan, BitmapHeapScan)) {
            per_row_audit_mmsearch_expression(
                reinterpret_cast<Node*>(reinterpret_cast<BitmapHeapScan*>(plan)->bitmapqualorig),
                audit.executable_qual_copies,
                oids);
        } else if (IsA(plan, TidScan)) {
            per_row_audit_mmsearch_expression(
                reinterpret_cast<Node*>(reinterpret_cast<TidScan*>(plan)->tidquals),
                audit.executable_qual_copies,
                oids);
        } else if (IsA(plan, TidRangeScan)) {
            per_row_audit_mmsearch_expression(
                reinterpret_cast<Node*>(reinterpret_cast<TidRangeScan*>(plan)->tidrangequals),
                audit.executable_qual_copies,
                oids);
        } else if (IsA(plan, ForeignScan)) {
            auto* scan = reinterpret_cast<ForeignScan*>(plan);
            for (List* expressions : {scan->fdw_exprs, scan->fdw_recheck_quals}) {
                per_row_audit_mmsearch_expression(
                    reinterpret_cast<Node*>(expressions), audit.executable_qual_copies, oids);
            }
        }

        if (IsA(plan, CustomScan)) {
            auto* custom = reinterpret_cast<CustomScan*>(plan);
            if (custom->methods == bioseq_column_scan_scan_methods() &&
                list_length(custom->custom_exprs) >= 3) {
                per_row_audit_mmsearch_expression(
                    reinterpret_cast<Node*>(list_nth(custom->custom_exprs, 0)),
                    audit.storage_qual_copies,
                    oids);
                per_row_audit_mmsearch_expression(
                    reinterpret_cast<Node*>(list_nth(custom->custom_exprs, 1)),
                    audit.storage_qual_copies,
                    oids);
                per_row_audit_mmsearch_expression(
                    reinterpret_cast<Node*>(list_nth(custom->custom_exprs, 2)),
                    audit.vector_qual_copies,
                    oids);
                for (int index = 3; index < list_length(custom->custom_exprs); ++index) {
                    Node* owner_expression =
                        reinterpret_cast<Node*>(list_nth(custom->custom_exprs, index));
                    if (exact_owner_candidate == nullptr) {
                        per_row_audit_mmsearch_expression(
                            owner_expression, audit.descendant_owner_copies, oids);
                    } else {
                        per_row_audit_exact_mmsearch_owner(owner_expression,
                                                           exact_owner_candidate,
                                                           audit.descendant_owner_copies,
                                                           oids);
                    }
                }
            } else {
                per_row_audit_mmsearch_expression(reinterpret_cast<Node*>(custom->custom_exprs),
                                                  audit.executable_qual_copies,
                                                  oids);
            }
            for (Plan* child : postgres_list<Plan>(custom->custom_plans)) {
                per_row_audit_mmsearch_plan(child, audit, oids, exact_owner_candidate);
            }
            return;
        }
        if (IsA(plan, Append)) {
            for (Plan* child : postgres_list<Plan>(reinterpret_cast<Append*>(plan)->appendplans)) {
                per_row_audit_mmsearch_plan(child, audit, oids, exact_owner_candidate);
            }
            return;
        }
        if (IsA(plan, MergeAppend)) {
            for (Plan* child :
                 postgres_list<Plan>(reinterpret_cast<MergeAppend*>(plan)->mergeplans)) {
                per_row_audit_mmsearch_plan(child, audit, oids, exact_owner_candidate);
            }
            return;
        }
        if (IsA(plan, BitmapAnd)) {
            for (Plan* child :
                 postgres_list<Plan>(reinterpret_cast<BitmapAnd*>(plan)->bitmapplans)) {
                per_row_audit_mmsearch_plan(child, audit, oids, exact_owner_candidate);
            }
            return;
        }
        if (IsA(plan, BitmapOr)) {
            for (Plan* child :
                 postgres_list<Plan>(reinterpret_cast<BitmapOr*>(plan)->bitmapplans)) {
                per_row_audit_mmsearch_plan(child, audit, oids, exact_owner_candidate);
            }
            return;
        }
        if (IsA(plan, SubqueryScan)) {
            per_row_audit_mmsearch_plan(
                reinterpret_cast<SubqueryScan*>(plan)->subplan, audit, oids, exact_owner_candidate);
            return;
        }
        per_row_audit_mmsearch_plan(outerPlan(plan), audit, oids, exact_owner_candidate);
        if (innerPlan(plan) != outerPlan(plan)) {
            per_row_audit_mmsearch_plan(innerPlan(plan), audit, oids, exact_owner_candidate);
        }
    }

    [[nodiscard]] Path* per_row_path_without_owned_qual(RelOptInfo* rel,
                                                        RestrictInfo* owned,
                                                        const bool require_bioseq_source) noexcept {
        if (rel == nullptr || owned == nullptr)
            return nullptr;
        Path* best = nullptr;
        for (Path* candidate : postgres_list<Path>(rel->pathlist)) {
            if (candidate == nullptr || candidate->parent != rel ||
                batch_join_path_contains_parallel_execution(candidate) ||
                per_row_descendant_owns_restriction(candidate, owned) ||
                (require_bioseq_source && !slice_project_path_tree_has_bioseq_scan(candidate)) ||
                !std::isfinite(candidate->total_cost) || candidate->total_cost < 0)
                continue;
            if (best == nullptr || candidate->total_cost < best->total_cost) {
                best = candidate;
            }
        }
        return best;
    }

    /* A reconstructed join may expose raw sequence Vars that its incumbent child
     * PathTargets did not need after core pushed the original join predicate.
     * Add only positive, level-zero raw Vars owned by this child's relids.  The
     * projection is a new Path node; shared child paths and PHV/whole-row
     * semantics are never mutated or synthesized. */
    [[nodiscard]] Path*
    per_row_project_required_child_vars(PlannerInfo* root, Path* child, List* required_vars) {
        if (root == nullptr || child == nullptr || child->parent == nullptr ||
            child->pathtarget == nullptr || required_vars == nullptr) {
            return nullptr;
        }
        PathTarget* projected = copy_pathtarget(child->pathtarget);
        bool added = false;
        for (Node* expression : postgres_list<Node>(required_vars)) {
            Var* var = try_cast_pg_node<Var>(expression);
            const bool row_identity = per_row_is_exact_row_identity_var(var);
            if (var == nullptr || !bioseq_is_concrete_rte_varno(var->varno) ||
                (var->varattno <= 0 && !row_identity) || var->varlevelsup != 0 ||
                !bms_is_empty(var->varnullingrels))
                return nullptr;
            if (!bms_is_member(static_cast<int>(var->varno), child->parent->relids))
                continue;
            if (!path_target_contains_expression(projected, reinterpret_cast<Expr*>(var))) {
                add_column_to_pathtarget(projected, reinterpret_cast<Expr*>(copyObject(var)), 0);
                added = true;
            }
        }
        if (!added)
            return child;
        set_pathtarget_cost_width(root, projected);
        return reinterpret_cast<Path*>(
            create_projection_path(root, child->parent, child, projected));
    }

    struct per_row_descendant_ownership {
        CustomPath* owner = nullptr;
        std::size_t matches = 0u;
        bool invalid_owner_abi = false;
        bool unsupported_wrapper = false;
    };

    [[nodiscard]] Node* per_row_known_path_owner_recipe(CustomPath* custom, bool& invalid) {
        if (custom == nullptr)
            return nullptr;
        const bool raw_owner = custom->methods == bioseq_batch_join_path_methods();
        const bool transformed_owner = custom->methods == bioseq_per_row_join_path_methods();
        if (!raw_owner && !transformed_owner)
            return nullptr;
        const int expected_version =
            raw_owner ? k_bioseq_batch_join_plan_version : k_bioseq_per_row_join_plan_version;
        const int expected_children = raw_owner ? 2 : 1;
        const int expected_fields = raw_owner ? k_bioseq_batch_join_path_private_fields
                                              : k_bioseq_per_row_join_path_private_fields;
        Node* version = list_length(custom->custom_private) == expected_fields
                            ? reinterpret_cast<Node*>(linitial(custom->custom_private))
                            : nullptr;
        Node* recipe = list_length(custom->custom_private) == expected_fields
                           ? reinterpret_cast<Node*>(list_nth(custom->custom_private, 4))
                           : nullptr;
        if (version == nullptr || !IsA(version, Integer) || intVal(version) != expected_version ||
            list_length(custom->custom_paths) != expected_children || recipe == nullptr ||
            exprType(recipe) != get_cached_stub_oids().bioseq_seq_type_oid) {
            invalid = true;
            return nullptr;
        }
        return recipe;
    }

    void per_row_analyze_descendant_exact_owner(Path* path,
                                                Node* helper_argument,
                                                per_row_descendant_ownership& ownership) {
        if (path == nullptr || helper_argument == nullptr || ownership.invalid_owner_abi ||
            ownership.unsupported_wrapper)
            return;
        check_stack_depth();
        if (IsA(path, CustomPath)) {
            auto* custom = reinterpret_cast<CustomPath*>(path);
            const bool known_owner = custom->methods == bioseq_batch_join_path_methods() ||
                                     custom->methods == bioseq_per_row_join_path_methods();
            if (known_owner) {
                Node* recipe = per_row_known_path_owner_recipe(custom, ownership.invalid_owner_abi);
                if (ownership.invalid_owner_abi)
                    return;
                if (recipe != nullptr && equal(helper_argument, recipe)) {
                    ++ownership.matches;
                    ownership.owner = custom;
                }
            } else if (custom->methods != bioseq_column_scan_path_methods() &&
                       custom->methods != &bioseq_slice_project_path_methods()) {
                /* Unknown CustomPaths can hide an evaluation/projection frontier in
                 * provider-private state.  Never infer helper provenance through one. */
                ownership.unsupported_wrapper = true;
                return;
            }
            for (Path* child : postgres_list<Path>(custom->custom_paths)) {
                per_row_analyze_descendant_exact_owner(child, helper_argument, ownership);
            }
            return;
        }
        auto descend_join = [helper_argument, &ownership](JoinPath* join) {
            if (join == nullptr)
                return;
            per_row_analyze_descendant_exact_owner(join->outerjoinpath, helper_argument, ownership);
            per_row_analyze_descendant_exact_owner(join->innerjoinpath, helper_argument, ownership);
        };
        if (IsA(path, NestPath)) {
            descend_join(&reinterpret_cast<NestPath*>(path)->jpath);
            return;
        }
        if (IsA(path, MergePath)) {
            descend_join(&reinterpret_cast<MergePath*>(path)->jpath);
            return;
        }
        if (IsA(path, HashPath)) {
            descend_join(&reinterpret_cast<HashPath*>(path)->jpath);
            return;
        }
        auto descend_one = [helper_argument, &ownership](Path* child) {
            per_row_analyze_descendant_exact_owner(child, helper_argument, ownership);
        };
        if (IsA(path, ProjectionPath)) {
            descend_one(reinterpret_cast<ProjectionPath*>(path)->subpath);
            return;
        }
        if (IsA(path, ProjectSetPath)) {
            descend_one(reinterpret_cast<ProjectSetPath*>(path)->subpath);
            return;
        }
        if (IsA(path, SortPath)) {
            descend_one(reinterpret_cast<SortPath*>(path)->subpath);
            return;
        }
        if (IsA(path, IncrementalSortPath)) {
            descend_one(reinterpret_cast<IncrementalSortPath*>(path)->spath.subpath);
            return;
        }
        if (IsA(path, MaterialPath)) {
            descend_one(reinterpret_cast<MaterialPath*>(path)->subpath);
            return;
        }
        if (IsA(path, MemoizePath)) {
            descend_one(reinterpret_cast<MemoizePath*>(path)->subpath);
            return;
        }
        if (IsA(path, UniquePath)) {
            descend_one(reinterpret_cast<UniquePath*>(path)->subpath);
            return;
        }
        if (IsA(path, GatherPath)) {
            descend_one(reinterpret_cast<GatherPath*>(path)->subpath);
            return;
        }
        if (IsA(path, GatherMergePath)) {
            descend_one(reinterpret_cast<GatherMergePath*>(path)->subpath);
            return;
        }
        auto descend_list = [helper_argument, &ownership](List* paths) {
            for (Path* child : postgres_list<Path>(paths)) {
                per_row_analyze_descendant_exact_owner(child, helper_argument, ownership);
            }
        };
        if (IsA(path, AppendPath)) {
            descend_list(reinterpret_cast<AppendPath*>(path)->subpaths);
            return;
        }
        if (IsA(path, MergeAppendPath)) {
            descend_list(reinterpret_cast<MergeAppendPath*>(path)->subpaths);
            return;
        }
        /* Ordinary base-relation scan paths have no child Path.  Anything else is
         * an unmodelled join/subquery/foreign wrapper and must fail closed. */
        if (path->parent == nullptr || (path->parent->reloptkind != RELOPT_BASEREL &&
                                        path->parent->reloptkind != RELOPT_OTHER_MEMBER_REL)) {
            ownership.unsupported_wrapper = true;
        }
    }

    [[nodiscard]] Path* per_row_clone_path_to_helper_owner(PlannerInfo* root,
                                                           Path* path,
                                                           CustomPath* owner,
                                                           Node* identity,
                                                           bool& applied,
                                                           bool& invalid);

    [[nodiscard]] PathTarget* per_row_helper_passthrough_target(PlannerInfo* root,
                                                                PathTarget* source,
                                                                Node* identity,
                                                                bool& invalid) {
        if (root == nullptr || source == nullptr || identity == nullptr) {
            invalid = true;
            return source;
        }
        std::vector<Index> sortgrouprefs;
        if (!per_row_authoritative_sortgrouprefs(root, source->exprs, sortgrouprefs) ||
            sortgrouprefs.size() != static_cast<std::size_t>(list_length(source->exprs))) {
            invalid = true;
            return source;
        }
        List* expressions = static_cast<List*>(copyObject(source->exprs));
        if (!per_row_expression_list_contains(expressions, identity)) {
            expressions = lappend(expressions, copyObject(identity));
            sortgrouprefs.push_back(0);
        }
        PathTarget* target = create_empty_pathtarget();
        target->exprs = expressions;
        const bool has_sortgroupref = std::any_of(sortgrouprefs.begin(),
                                                  sortgrouprefs.end(),
                                                  [](Index value) noexcept { return value != 0; });
        if (has_sortgroupref) {
            target->sortgrouprefs =
                static_cast<Index*>(palloc0(sortgrouprefs.size() * sizeof(Index)));
            for (std::size_t index = 0u; index < sortgrouprefs.size(); ++index) {
                target->sortgrouprefs[index] = sortgrouprefs[index];
            }
        }
        set_pathtarget_cost_width(root, target);
        return target;
    }

    [[nodiscard]] List* per_row_clone_path_list_to_helper_owner(PlannerInfo* root,
                                                                List* paths,
                                                                CustomPath* owner,
                                                                Node* identity,
                                                                bool& applied,
                                                                bool& invalid) {
        List* result = NIL;
        bool changed = false;
        for (Path* child : postgres_list<Path>(paths)) {
            Path* replacement = applied || invalid
                                    ? child
                                    : per_row_clone_path_to_helper_owner(
                                          root, child, owner, identity, applied, invalid);
            changed = changed || replacement != child;
            result = lappend(result, replacement);
        }
        return changed ? result : paths;
    }

    [[nodiscard]] Path* per_row_clone_path_to_helper_owner(PlannerInfo* root,
                                                           Path* path,
                                                           CustomPath* owner,
                                                           Node* identity,
                                                           bool& applied,
                                                           bool& invalid) {
        if (path == nullptr || owner == nullptr || identity == nullptr || applied || invalid) {
            return path;
        }
        if (path == reinterpret_cast<Path*>(owner)) {
            auto* clone = makeNode(CustomPath);
            *clone = *owner;
            clone->path.pathtarget =
                per_row_helper_passthrough_target(root, path->pathtarget, identity, invalid);
            if (invalid)
                return path;
            applied = true;
            return reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, CustomPath)) {
            auto* source = reinterpret_cast<CustomPath*>(path);
            List* children = per_row_clone_path_list_to_helper_owner(
                root, source->custom_paths, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            auto* clone = makeNode(CustomPath);
            *clone = *source;
            clone->custom_paths = children;
            clone->path.pathtarget =
                per_row_helper_passthrough_target(root, source->path.pathtarget, identity, invalid);
            if (invalid)
                return path;
            return reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, NestPath)) {
            auto* source = reinterpret_cast<NestPath*>(path);
            auto* clone = makeNode(NestPath);
            *clone = *source;
            clone->jpath.outerjoinpath = per_row_clone_path_to_helper_owner(
                root, source->jpath.outerjoinpath, owner, identity, applied, invalid);
            clone->jpath.innerjoinpath = per_row_clone_path_to_helper_owner(
                root, source->jpath.innerjoinpath, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            clone->jpath.path.pathtarget = per_row_helper_passthrough_target(
                root, source->jpath.path.pathtarget, identity, invalid);
            return invalid ? path : reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, MergePath)) {
            auto* source = reinterpret_cast<MergePath*>(path);
            auto* clone = makeNode(MergePath);
            *clone = *source;
            clone->jpath.outerjoinpath = per_row_clone_path_to_helper_owner(
                root, source->jpath.outerjoinpath, owner, identity, applied, invalid);
            clone->jpath.innerjoinpath = per_row_clone_path_to_helper_owner(
                root, source->jpath.innerjoinpath, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            clone->jpath.path.pathtarget = per_row_helper_passthrough_target(
                root, source->jpath.path.pathtarget, identity, invalid);
            return invalid ? path : reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, HashPath)) {
            auto* source = reinterpret_cast<HashPath*>(path);
            auto* clone = makeNode(HashPath);
            *clone = *source;
            clone->jpath.outerjoinpath = per_row_clone_path_to_helper_owner(
                root, source->jpath.outerjoinpath, owner, identity, applied, invalid);
            clone->jpath.innerjoinpath = per_row_clone_path_to_helper_owner(
                root, source->jpath.innerjoinpath, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            clone->jpath.path.pathtarget = per_row_helper_passthrough_target(
                root, source->jpath.path.pathtarget, identity, invalid);
            return invalid ? path : reinterpret_cast<Path*>(clone);
        }
#define BIOSEQ_CLONE_HELPER_UNARY_PATH(PathType, ChildMember)                                      \
    if (IsA(path, PathType)) {                                                                     \
        auto* source = reinterpret_cast<PathType*>(path);                                          \
        Path* child = per_row_clone_path_to_helper_owner(                                          \
            root, source->ChildMember, owner, identity, applied, invalid);                         \
        if (!applied || invalid)                                                                   \
            return path;                                                                           \
        auto* clone = makeNode(PathType);                                                          \
        *clone = *source;                                                                          \
        clone->ChildMember = child;                                                                \
        reinterpret_cast<Path*>(clone)->pathtarget =                                               \
            per_row_helper_passthrough_target(root, path->pathtarget, identity, invalid);          \
        return invalid ? path : reinterpret_cast<Path*>(clone);                                    \
    }
        BIOSEQ_CLONE_HELPER_UNARY_PATH(ProjectionPath, subpath)
        BIOSEQ_CLONE_HELPER_UNARY_PATH(ProjectSetPath, subpath)
        BIOSEQ_CLONE_HELPER_UNARY_PATH(SortPath, subpath)
        BIOSEQ_CLONE_HELPER_UNARY_PATH(MaterialPath, subpath)
        BIOSEQ_CLONE_HELPER_UNARY_PATH(MemoizePath, subpath)
        BIOSEQ_CLONE_HELPER_UNARY_PATH(UniquePath, subpath)
        BIOSEQ_CLONE_HELPER_UNARY_PATH(GatherPath, subpath)
        BIOSEQ_CLONE_HELPER_UNARY_PATH(GatherMergePath, subpath)
#undef BIOSEQ_CLONE_HELPER_UNARY_PATH
        if (IsA(path, IncrementalSortPath)) {
            auto* source = reinterpret_cast<IncrementalSortPath*>(path);
            Path* child = per_row_clone_path_to_helper_owner(
                root, source->spath.subpath, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            auto* clone = makeNode(IncrementalSortPath);
            *clone = *source;
            clone->spath.subpath = child;
            clone->spath.path.pathtarget = per_row_helper_passthrough_target(
                root, source->spath.path.pathtarget, identity, invalid);
            return invalid ? path : reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, AppendPath)) {
            auto* source = reinterpret_cast<AppendPath*>(path);
            List* children = per_row_clone_path_list_to_helper_owner(
                root, source->subpaths, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            auto* clone = makeNode(AppendPath);
            *clone = *source;
            clone->subpaths = children;
            clone->path.pathtarget =
                per_row_helper_passthrough_target(root, source->path.pathtarget, identity, invalid);
            return invalid ? path : reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, MergeAppendPath)) {
            auto* source = reinterpret_cast<MergeAppendPath*>(path);
            List* children = per_row_clone_path_list_to_helper_owner(
                root, source->subpaths, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            auto* clone = makeNode(MergeAppendPath);
            *clone = *source;
            clone->subpaths = children;
            clone->path.pathtarget =
                per_row_helper_passthrough_target(root, source->path.pathtarget, identity, invalid);
            return invalid ? path : reinterpret_cast<Path*>(clone);
        }
        return path;
    }

    /* A reconstructed transformed owner is deliberately narrower than the
     * incumbent join relation: helpers owned by the new PerRow node must not leak
     * into a descendant BioseqScan or an older BatchJoin.  PostgreSQL Path nodes
     * are shared, so normalize only after slice_project_clone_mutable_path_spine()
     * has made every supported container private.  Availability is scoped to the
     * current subtree: a descendant-owned helper is retained at its owner and
     * above it, but is removed from every child below that owner. */
    struct per_row_base_helper_ownership {
        CustomPath* owner = nullptr;
        std::size_t matches = 0u;
        bool invalid_owner_abi = false;
        bool unsupported_wrapper = false;
    };

    [[nodiscard]] FuncExpr*
    per_row_helper_identity_function(Node* identity, bioseq_stub_oids const& oids) noexcept {
        if (PlaceHolderVar* placeholder = try_cast_pg_node<PlaceHolderVar>(identity)) {
            return batch_join_match_helper_phv_function(placeholder, oids);
        }
        return batch_join_well_formed_match_helper(identity, oids);
    }

    [[nodiscard]] Node* per_row_base_scan_owner_recipe(CustomPath* custom, bool& invalid) {
        if (custom == nullptr || custom->methods != bioseq_column_scan_path_methods()) {
            return nullptr;
        }
        Node* recipe = nullptr;
        for (Node* private_node : postgres_list<Node>(custom->custom_private)) {
            if (private_node == nullptr || !IsA(private_node, List))
                continue;
            List* tag = reinterpret_cast<List*>(private_node);
            Node* magic = list_length(tag) >= 1 ? reinterpret_cast<Node*>(linitial(tag)) : nullptr;
            if (magic == nullptr || !IsA(magic, Integer) ||
                intVal(magic) != k_bioseq_mmsearch_owner_magic)
                continue;
            Node* version =
                list_length(tag) == 3 ? reinterpret_cast<Node*>(list_nth(tag, 1)) : nullptr;
            Node* expressions_node =
                list_length(tag) == 3 ? reinterpret_cast<Node*>(list_nth(tag, 2)) : nullptr;
            List* expressions = expressions_node != nullptr && IsA(expressions_node, List)
                                    ? reinterpret_cast<List*>(expressions_node)
                                    : NIL;
            FuncExpr* mmsearch =
                expressions == NIL
                    ? nullptr
                    : try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(linitial(expressions)));
            if (recipe != nullptr || version == nullptr || !IsA(version, Integer) ||
                intVal(version) != k_bioseq_mmsearch_owner_version || mmsearch == nullptr ||
                list_length(expressions) > 2 ||
                !is_mmsearch_funcid(mmsearch->funcid, get_cached_stub_oids()) ||
                (list_length(mmsearch->args) != 2 && list_length(mmsearch->args) != 3) ||
                list_length(custom->custom_restrictinfo) != list_length(expressions)) {
                invalid = true;
                return nullptr;
            }
            for (Node* expression : postgres_list<Node>(expressions)) {
                FuncExpr* owned = try_cast_pg_node<FuncExpr>(expression);
                if (owned == nullptr ||
                    !is_mmsearch_funcid(owned->funcid, get_cached_stub_oids()) ||
                    (list_length(owned->args) != 2 && list_length(owned->args) != 3)) {
                    invalid = true;
                    return nullptr;
                }
            }
            recipe = reinterpret_cast<Node*>(linitial(mmsearch->args));
            if (recipe == nullptr ||
                exprType(recipe) != get_cached_stub_oids().bioseq_seq_type_oid) {
                invalid = true;
                return nullptr;
            }
        }
        return recipe;
    }

    struct exact_parameterized_bmo_ownership {
        std::size_t matches = 0u;
        bool invalid = false;
        std::uint32_t rejection_mask = 0u;
        std::size_t wrapper_owns_expected = 0u;
        std::uint32_t wrapper_method_mask = 0u;
        std::size_t wrapper_restriction_count = 0u;
        std::size_t wrapper_child_count = 0u;
        std::size_t wrapper_version = 0u;
        std::size_t wrapper_pointer_matches = 0u;
        std::size_t wrapper_serial_matches = 0u;
        std::size_t wrapper_clause_matches = 0u;
        std::size_t expected_rinfo_serial = 0u;
        std::size_t wrapper_rinfo_serial = 0u;
        std::size_t expected_target_varno = 0u;
        std::size_t expected_query_varno = 0u;
        std::size_t wrapper_private_target_varno = 0u;
        std::size_t wrapper_private_query_varno = 0u;
        std::size_t wrapper_private_target_equal = 0u;
        std::size_t wrapper_private_query_equal = 0u;
        std::size_t wrapper_owner0_target_varno = 0u;
        std::size_t wrapper_owner0_query_varno = 0u;
        std::size_t wrapper_owner1_target_varno = 0u;
        std::size_t wrapper_owner1_query_varno = 0u;
        std::uint64_t wrapper_parent_relids_low = 0u;
        std::uint64_t wrapper_child0_relids_low = 0u;
        std::uint64_t wrapper_child1_relids_low = 0u;
    };

    [[nodiscard]] std::uint64_t exact_bmo_audit_relids_low(Relids relids) noexcept {
        std::uint64_t mask = 0u;
        int relid = -1;
        while ((relid = bms_next_member(relids, relid)) >= 0) {
            if (relid < 64)
                mask |= std::uint64_t{1} << relid;
        }
        return mask;
    }

    /* PostgreSQL copies RestrictInfo nodes between baserel parameterization and
     * join-path construction while preserving rinfo_serial specifically so the
     * copies retain one planner identity.  Pointer equality alone therefore
     * misses the BMO path that owns the same qual.  Admit only that documented
     * clone relation plus an exact executable/security shape; equal SQL text with
     * a different serial remains a distinct restriction. */
    [[nodiscard]] bool same_planner_restriction_identity(RestrictInfo* candidate,
                                                         RestrictInfo* expected) noexcept {
        if (candidate == expected) {
            return candidate != nullptr && candidate->rinfo_serial > 0;
        }
        return candidate != nullptr && expected != nullptr && candidate->rinfo_serial > 0 &&
               expected->rinfo_serial > 0 && candidate->rinfo_serial == expected->rinfo_serial &&
               candidate->pseudoconstant == expected->pseudoconstant &&
               candidate->security_level == expected->security_level &&
               candidate->is_pushed_down == expected->is_pushed_down &&
               candidate->can_join == expected->can_join &&
               candidate->has_clone == expected->has_clone &&
               candidate->is_clone == expected->is_clone &&
               candidate->leakproof == expected->leakproof &&
               candidate->has_volatile == expected->has_volatile &&
               candidate->num_base_rels == expected->num_base_rels &&
               bms_equal(candidate->clause_relids, expected->clause_relids) &&
               bms_equal(candidate->required_relids, expected->required_relids) &&
               bms_equal(candidate->incompatible_relids, expected->incompatible_relids) &&
               bms_equal(candidate->outer_relids, expected->outer_relids) &&
               bms_equal(candidate->left_relids, expected->left_relids) &&
               bms_equal(candidate->right_relids, expected->right_relids) &&
               equal(candidate->orclause, expected->orclause) &&
               equal(candidate->clause, expected->clause);
    }

    enum exact_parameterized_bmo_owner_rejection : std::uint32_t {
        k_exact_bmo_unknown_custom_path = 1u << 0,
        k_exact_bmo_base_scan_has_children = 1u << 1,
        k_exact_bmo_owner_abi = 1u << 2,
        k_exact_bmo_restriction_multiplicity = 1u << 3,
        k_exact_bmo_missing_param_info = 1u << 4,
        k_exact_bmo_missing_ppi_clause = 1u << 5,
        k_exact_bmo_nested_join_clause = 1u << 6,
        k_exact_bmo_unknown_core_path = 1u << 7,
        k_exact_bmo_known_wrapper_abi = 1u << 8,
        k_exact_bmo_known_wrapper_owns_clause = 1u << 9,
    };

    inline void reject_exact_parameterized_bmo_owner(
        exact_parameterized_bmo_ownership& ownership,
        const exact_parameterized_bmo_owner_rejection reason) noexcept {
        ownership.invalid = true;
        ownership.rejection_mask |= static_cast<std::uint32_t>(reason);
    }

    [[nodiscard]] bool foreign_known_join_owner_is_valid(PlannerInfo* root, CustomPath* custom) {
        if (root == nullptr || custom == nullptr)
            return false;
        const bool raw_owner = custom->methods == bioseq_batch_join_path_methods();
        const bool transformed_owner = custom->methods == bioseq_per_row_join_path_methods();
        if (!raw_owner && !transformed_owner)
            return false;
        bool invalid_owner_abi = false;
        if (per_row_known_path_owner_recipe(custom, invalid_owner_abi) == nullptr ||
            invalid_owner_abi || custom->path.parent == nullptr)
            return false;

        FuncExpr* owned_expression = nullptr;
        std::size_t semantic_calls = 0u;
        for (RestrictInfo* owned : postgres_list<RestrictInfo>(custom->custom_restrictinfo)) {
            const std::size_t calls =
                owned == nullptr
                    ? 0u
                    : count_mmsearch_occurrences(reinterpret_cast<Node*>(owned->clause),
                                                 get_cached_stub_oids());
            semantic_calls += calls;
            if (calls != 0u) {
                if (calls != 1u || owned_expression != nullptr)
                    return false;
                owned_expression =
                    try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(owned->clause));
                if (owned_expression == nullptr)
                    return false;
            }
        }
        if (semantic_calls != 1u || owned_expression == nullptr)
            return false;
        const int executable_index = raw_owner ? 8 : 6;
        Node* private_target = reinterpret_cast<Node*>(list_nth(custom->custom_private, 4));
        Node* private_query = reinterpret_cast<Node*>(list_nth(custom->custom_private, 5));
        FuncExpr* executable_expression =
            list_length(custom->custom_private) > executable_index
                ? try_cast_pg_node<FuncExpr>(
                      reinterpret_cast<Node*>(list_nth(custom->custom_private, executable_index)))
                : nullptr;
        if (executable_expression == nullptr ||
            !is_mmsearch_funcid(executable_expression->funcid, get_cached_stub_oids()) ||
            count_mmsearch_occurrences(private_target, get_cached_stub_oids()) != 0u ||
            count_mmsearch_occurrences(private_query, get_cached_stub_oids()) != 0u ||
            count_mmsearch_occurrences(reinterpret_cast<Node*>(executable_expression),
                                       get_cached_stub_oids()) != 1u)
            return false;
        FuncExpr* normalized_owned = copyObject(owned_expression);
        if (list_length(normalized_owned->args) == 3) {
            Node* option = reinterpret_cast<Node*>(list_nth(normalized_owned->args, 2));
            Node* folded =
                eval_const_expressions(root, reinterpret_cast<Node*>(copyObject(option)));
            Const* folded_const = try_cast_pg_node<Const>(folded);
            if (folded_const == nullptr ||
                folded_const->consttype != get_cached_stub_oids().bioseq_aligner_options_type_oid) {
                return false;
            }
            lfirst(list_nth_cell(normalized_owned->args, 2)) = folded;
        }
        if (!equal(normalized_owned, executable_expression))
            return false;

        List* variables = pull_var_clause(reinterpret_cast<Node*>(owned_expression),
                                          PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS |
                                              PVC_RECURSE_PLACEHOLDERS);
        for (Node* variable : postgres_list<Node>(variables)) {
            Var* var = try_cast_pg_node<Var>(variable);
            if (var == nullptr || !bioseq_is_concrete_rte_varno(var->varno) ||
                var->varlevelsup != 0 || var->varattno <= 0 || !bms_is_empty(var->varnullingrels) ||
                !bms_is_member(static_cast<int>(var->varno), custom->path.parent->relids))
                return false;
        }

        bool children_cover_parent = raw_owner ? list_length(custom->custom_paths) == 2
                                               : list_length(custom->custom_paths) == 1;
        if (children_cover_parent && raw_owner) {
            Path* first = reinterpret_cast<Path*>(linitial(custom->custom_paths));
            Path* second = reinterpret_cast<Path*>(lsecond(custom->custom_paths));
            children_cover_parent = first != nullptr && second != nullptr &&
                                    first->parent != nullptr && second->parent != nullptr &&
                                    !bms_overlap(first->parent->relids, second->parent->relids);
            Relids child_union = children_cover_parent
                                     ? bms_union(first->parent->relids, second->parent->relids)
                                     : nullptr;
            children_cover_parent =
                children_cover_parent && bms_equal(child_union, custom->path.parent->relids);
            bms_free(child_union);
        } else if (children_cover_parent) {
            Path* child = reinterpret_cast<Path*>(linitial(custom->custom_paths));
            children_cover_parent = child != nullptr && child->parent != nullptr &&
                                    bms_equal(child->parent->relids, custom->path.parent->relids);
        }
        return children_cover_parent && !custom->path.parallel_aware &&
               !custom->path.parallel_safe && custom->path.parallel_workers == 0 &&
               std::isfinite(custom->path.rows) && custom->path.rows >= 0 &&
               std::isfinite(custom->path.startup_cost) && custom->path.startup_cost >= 0 &&
               std::isfinite(custom->path.total_cost) &&
               custom->path.total_cost >= custom->path.startup_cost;
    }

    void count_exact_parameterized_bmo_owners(PlannerInfo* root,
                                              Path* path,
                                              RestrictInfo* restriction,
                                              exact_parameterized_bmo_ownership& ownership,
                                              const bool admit_known_wrapper_owner = false) {
        if (root == nullptr || path == nullptr || restriction == nullptr || ownership.invalid)
            return;
        check_stack_depth();
        if (admit_known_wrapper_owner) {
            for (Node* expression :
                 postgres_list<Node>(path->pathtarget == nullptr ? NIL : path->pathtarget->exprs)) {
                if (count_mmsearch_occurrences(expression, get_cached_stub_oids()) != 0u) {
                    reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_nested_join_clause);
                    return;
                }
            }
            for (RestrictInfo* parameter_clause : postgres_list<RestrictInfo>(
                     path->param_info == nullptr ? NIL : path->param_info->ppi_clauses)) {
                if (parameter_clause == nullptr ||
                    count_mmsearch_occurrences(reinterpret_cast<Node*>(parameter_clause->clause),
                                               get_cached_stub_oids()) == 0u)
                    continue;
                bool owned_here = false;
                if (IsA(path, CustomPath)) {
                    for (RestrictInfo* owned : postgres_list<RestrictInfo>(
                             reinterpret_cast<CustomPath*>(path)->custom_restrictinfo)) {
                        owned_here = owned_here ||
                                     same_planner_restriction_identity(owned, parameter_clause);
                    }
                }
                if (!owned_here) {
                    reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_nested_join_clause);
                    return;
                }
            }
        }
        if (IsA(path, CustomPath)) {
            auto* custom = reinterpret_cast<CustomPath*>(path);
            if (custom->methods == bioseq_column_scan_path_methods() &&
                custom->custom_paths != NIL) {
                /* A BioseqColumnScan is a leaf.  Children would make the owner count
                 * incomplete and therefore invalidate this internal path ABI. */
                reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_base_scan_has_children);
                return;
            }
            bool owns_expected_restriction = false;
            for (RestrictInfo* owned : postgres_list<RestrictInfo>(custom->custom_restrictinfo)) {
                owns_expected_restriction = owns_expected_restriction ||
                                            same_planner_restriction_identity(owned, restriction);
            }
            if (admit_known_wrapper_owner && custom->methods == bioseq_column_scan_path_methods()) {
                bool invalid_owner_abi = false;
                Node* recipe = per_row_base_scan_owner_recipe(custom, invalid_owner_abi);
                if (custom->custom_restrictinfo == NIL) {
                    if (recipe != nullptr || invalid_owner_abi) {
                        reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_owner_abi);
                        return;
                    }
                } else {
                    if (recipe == nullptr || invalid_owner_abi) {
                        reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_owner_abi);
                        return;
                    }
                    for (RestrictInfo* owned :
                         postgres_list<RestrictInfo>(custom->custom_restrictinfo)) {
                        if (owned == nullptr ||
                            count_mmsearch_occurrences(reinterpret_cast<Node*>(owned->clause),
                                                       get_cached_stub_oids()) != 1u ||
                            try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(owned->clause)) ==
                                nullptr) {
                            reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_owner_abi);
                            return;
                        }
                    }
                    if (custom->path.parallel_aware || custom->path.parallel_safe ||
                        custom->path.parallel_workers != 0 || !std::isfinite(custom->path.rows) ||
                        custom->path.rows < 0 || !std::isfinite(custom->path.startup_cost) ||
                        custom->path.startup_cost < 0 || !std::isfinite(custom->path.total_cost) ||
                        custom->path.total_cost < custom->path.startup_cost) {
                        reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_owner_abi);
                        return;
                    }
                }
                if (list_length(custom->custom_private) < 2) {
                    reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_owner_abi);
                    return;
                }
                for (int index = 0; index < 2; ++index) {
                    Node* residual_node =
                        reinterpret_cast<Node*>(list_nth(custom->custom_private, index));
                    if (residual_node != nullptr && !IsA(residual_node, List)) {
                        reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_owner_abi);
                        return;
                    }
                    for (RestrictInfo* residual : postgres_list<RestrictInfo>(
                             residual_node == nullptr ? NIL
                                                      : reinterpret_cast<List*>(residual_node))) {
                        if (residual != nullptr &&
                            count_mmsearch_occurrences(reinterpret_cast<Node*>(residual->clause),
                                                       get_cached_stub_oids()) != 0u) {
                            reject_exact_parameterized_bmo_owner(ownership,
                                                                 k_exact_bmo_nested_join_clause);
                            return;
                        }
                    }
                }
            }
            if (custom->methods == bioseq_column_scan_path_methods() && owns_expected_restriction) {
                std::size_t exact_restriction_count = 0u;
                for (RestrictInfo* owned :
                     postgres_list<RestrictInfo>(custom->custom_restrictinfo)) {
                    if (same_planner_restriction_identity(owned, restriction))
                        ++exact_restriction_count;
                }
                bool invalid_owner_abi = false;
                Node* recipe = per_row_base_scan_owner_recipe(custom, invalid_owner_abi);
                if (invalid_owner_abi || recipe == nullptr) {
                    reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_owner_abi);
                    return;
                }
                if (exact_restriction_count != 1u) {
                    reject_exact_parameterized_bmo_owner(ownership,
                                                         k_exact_bmo_restriction_multiplicity);
                    return;
                }
                if (custom->path.param_info == nullptr) {
                    reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_missing_param_info);
                    return;
                }
                std::size_t ppi_exact_count = 0u;
                for (RestrictInfo* parameter_clause :
                     postgres_list<RestrictInfo>(custom->path.param_info->ppi_clauses)) {
                    if (same_planner_restriction_identity(parameter_clause, restriction))
                        ++ppi_exact_count;
                }
                if (ppi_exact_count != 1u) {
                    reject_exact_parameterized_bmo_owner(
                        ownership,
                        ppi_exact_count == 0u ? k_exact_bmo_missing_ppi_clause
                                              : k_exact_bmo_restriction_multiplicity);
                    return;
                }
                ++ownership.matches;
                return;
            }
            if (custom->methods == bioseq_column_scan_path_methods())
                return;

            const bool known_join_owner = custom->methods == bioseq_batch_join_path_methods() ||
                                          custom->methods == bioseq_per_row_join_path_methods();
            const bool known_slice_wrapper =
                custom->methods == &bioseq_slice_project_path_methods();
            if (!known_join_owner && !known_slice_wrapper) {
                /* Unknown CustomPaths can hide restrictions or private children. */
                reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_unknown_custom_path);
                return;
            }
            if (admit_known_wrapper_owner && known_join_owner &&
                !foreign_known_join_owner_is_valid(root, custom)) {
                reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_known_wrapper_abi);
                return;
            }
            if (owns_expected_restriction) {
                ++ownership.wrapper_owns_expected;
                ownership.wrapper_method_mask |=
                    custom->methods == bioseq_batch_join_path_methods()
                        ? 1u
                        : (custom->methods == bioseq_per_row_join_path_methods()
                               ? 2u
                               : (known_slice_wrapper ? 4u : 0u));
                ownership.wrapper_restriction_count +=
                    static_cast<std::size_t>(list_length(custom->custom_restrictinfo));
                ownership.wrapper_child_count +=
                    static_cast<std::size_t>(list_length(custom->custom_paths));
                Node* private_version =
                    custom->custom_private == NIL
                        ? nullptr
                        : reinterpret_cast<Node*>(linitial(custom->custom_private));
                ownership.wrapper_version =
                    private_version != nullptr && IsA(private_version, Integer)
                        ? static_cast<std::size_t>(intVal(private_version))
                        : 0u;
                ownership.expected_rinfo_serial =
                    static_cast<std::size_t>(restriction->rinfo_serial);
                ownership.wrapper_parent_relids_low = exact_bmo_audit_relids_low(
                    custom->path.parent == nullptr ? nullptr : custom->path.parent->relids);
                if (list_length(custom->custom_paths) >= 1) {
                    Path* child = reinterpret_cast<Path*>(linitial(custom->custom_paths));
                    ownership.wrapper_child0_relids_low = exact_bmo_audit_relids_low(
                        child == nullptr || child->parent == nullptr ? nullptr
                                                                     : child->parent->relids);
                }
                if (list_length(custom->custom_paths) >= 2) {
                    Path* child = reinterpret_cast<Path*>(lsecond(custom->custom_paths));
                    ownership.wrapper_child1_relids_low = exact_bmo_audit_relids_low(
                        child == nullptr || child->parent == nullptr ? nullptr
                                                                     : child->parent->relids);
                }
                FuncExpr* expected_mmsearch =
                    try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(restriction->clause));
                Node* expected_target =
                    expected_mmsearch != nullptr && list_length(expected_mmsearch->args) >= 2
                        ? reinterpret_cast<Node*>(linitial(expected_mmsearch->args))
                        : nullptr;
                Node* expected_query =
                    expected_mmsearch != nullptr && list_length(expected_mmsearch->args) >= 2
                        ? reinterpret_cast<Node*>(lsecond(expected_mmsearch->args))
                        : nullptr;
                Var* expected_target_var = try_cast_pg_node<Var>(expected_target);
                Var* expected_query_var = try_cast_pg_node<Var>(expected_query);
                ownership.expected_target_varno =
                    expected_target_var == nullptr
                        ? 0u
                        : static_cast<std::size_t>(expected_target_var->varno);
                ownership.expected_query_varno =
                    expected_query_var == nullptr
                        ? 0u
                        : static_cast<std::size_t>(expected_query_var->varno);
                Node* private_target = nullptr;
                Node* private_query = nullptr;
                if (known_join_owner && list_length(custom->custom_private) >= 6) {
                    private_target = reinterpret_cast<Node*>(list_nth(custom->custom_private, 4));
                    private_query = reinterpret_cast<Node*>(list_nth(custom->custom_private, 5));
                }
                Var* private_target_var = try_cast_pg_node<Var>(private_target);
                Var* private_query_var = try_cast_pg_node<Var>(private_query);
                ownership.wrapper_private_target_varno =
                    private_target_var == nullptr
                        ? 0u
                        : static_cast<std::size_t>(private_target_var->varno);
                ownership.wrapper_private_query_varno =
                    private_query_var == nullptr
                        ? 0u
                        : static_cast<std::size_t>(private_query_var->varno);
                ownership.wrapper_private_target_equal =
                    private_target != nullptr && equal(private_target, expected_target) ? 1u : 0u;
                ownership.wrapper_private_query_equal =
                    private_query != nullptr && equal(private_query, expected_query) ? 1u : 0u;
                std::size_t owner_index = 0u;
                for (RestrictInfo* owned :
                     postgres_list<RestrictInfo>(custom->custom_restrictinfo)) {
                    if (owned == restriction)
                        ++ownership.wrapper_pointer_matches;
                    if (owned != nullptr && owned->rinfo_serial > 0 &&
                        owned->rinfo_serial == restriction->rinfo_serial) {
                        ++ownership.wrapper_serial_matches;
                    }
                    if (owned != nullptr && equal(owned->clause, restriction->clause)) {
                        ++ownership.wrapper_clause_matches;
                    }
                    if (same_planner_restriction_identity(owned, restriction)) {
                        ownership.wrapper_rinfo_serial =
                            static_cast<std::size_t>(owned->rinfo_serial);
                    }
                    FuncExpr* owned_mmsearch =
                        owned == nullptr
                            ? nullptr
                            : try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(owned->clause));
                    Var* owned_target =
                        owned_mmsearch != nullptr && list_length(owned_mmsearch->args) >= 2
                            ? try_cast_pg_node<Var>(
                                  reinterpret_cast<Node*>(linitial(owned_mmsearch->args)))
                            : nullptr;
                    Var* owned_query =
                        owned_mmsearch != nullptr && list_length(owned_mmsearch->args) >= 2
                            ? try_cast_pg_node<Var>(
                                  reinterpret_cast<Node*>(lsecond(owned_mmsearch->args)))
                            : nullptr;
                    if (owner_index == 0u) {
                        ownership.wrapper_owner0_target_varno =
                            owned_target == nullptr ? 0u
                                                    : static_cast<std::size_t>(owned_target->varno);
                        ownership.wrapper_owner0_query_varno =
                            owned_query == nullptr ? 0u
                                                   : static_cast<std::size_t>(owned_query->varno);
                    } else if (owner_index == 1u) {
                        ownership.wrapper_owner1_target_varno =
                            owned_target == nullptr ? 0u
                                                    : static_cast<std::size_t>(owned_target->varno);
                        ownership.wrapper_owner1_query_varno =
                            owned_query == nullptr ? 0u
                                                   : static_cast<std::size_t>(owned_query->varno);
                    }
                    ++owner_index;
                }
                if (!admit_known_wrapper_owner || !known_join_owner) {
                    reject_exact_parameterized_bmo_owner(ownership,
                                                         k_exact_bmo_known_wrapper_owns_clause);
                    return;
                }
                bool invalid_wrapper_abi = false;
                Node* owner_recipe = per_row_known_path_owner_recipe(custom, invalid_wrapper_abi);
                std::size_t exact_owner_count = 0u;
                std::size_t wrapper_mmsearch_count = 0u;
                FuncExpr* owned_mmsearch = nullptr;
                for (RestrictInfo* owned :
                     postgres_list<RestrictInfo>(custom->custom_restrictinfo)) {
                    const std::size_t calls =
                        owned == nullptr
                            ? 0u
                            : count_mmsearch_occurrences(reinterpret_cast<Node*>(owned->clause),
                                                         get_cached_stub_oids());
                    if (same_planner_restriction_identity(owned, restriction)) {
                        ++exact_owner_count;
                        owned_mmsearch =
                            calls == 1u
                                ? try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(owned->clause))
                                : nullptr;
                    }
                    wrapper_mmsearch_count += calls;
                }
                const int executable_index =
                    custom->methods == bioseq_batch_join_path_methods() ? 8 : 6;
                FuncExpr* executable_mmsearch =
                    list_length(custom->custom_private) > executable_index
                        ? try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(
                              list_nth(custom->custom_private, executable_index)))
                        : nullptr;
                postgres_list<Node> expected_operands(
                    expected_mmsearch == nullptr ? NIL : expected_mmsearch->args);
                postgres_list<Node> executable_operands(
                    executable_mmsearch == nullptr ? NIL : executable_mmsearch->args);
                const bool executable_shape =
                    executable_mmsearch != nullptr &&
                    is_mmsearch_funcid(executable_mmsearch->funcid, get_cached_stub_oids()) &&
                    (list_length(executable_mmsearch->args) == 2 ||
                     list_length(executable_mmsearch->args) == 3) &&
                    expected_operands.size() >= 2 && executable_operands.size() >= 2 &&
                    equal(executable_operands.get_first(), expected_operands.get_first()) &&
                    equal(executable_operands.get_second(), expected_operands.get_second());
                Var* owner_target_var = expected_operands.size() >= 2
                                            ? try_cast_pg_node<Var>(expected_operands.get_first())
                                            : nullptr;
                Var* owner_query_var = expected_operands.size() >= 2
                                           ? try_cast_pg_node<Var>(expected_operands.get_second())
                                           : nullptr;
                const bool owner_parent_contains_operands =
                    custom->path.parent != nullptr && owner_target_var != nullptr &&
                    owner_query_var != nullptr &&
                    bms_is_member(static_cast<int>(owner_target_var->varno),
                                  custom->path.parent->relids) &&
                    bms_is_member(static_cast<int>(owner_query_var->varno),
                                  custom->path.parent->relids);
                bool owner_children_cover_parent =
                    custom->path.parent != nullptr &&
                    ((custom->methods == bioseq_batch_join_path_methods() &&
                      list_length(custom->custom_paths) == 2) ||
                     (custom->methods == bioseq_per_row_join_path_methods() &&
                      list_length(custom->custom_paths) == 1));
                if (owner_children_cover_parent &&
                    custom->methods == bioseq_batch_join_path_methods()) {
                    Path* first_child = reinterpret_cast<Path*>(linitial(custom->custom_paths));
                    Path* second_child = reinterpret_cast<Path*>(lsecond(custom->custom_paths));
                    owner_children_cover_parent =
                        first_child != nullptr && second_child != nullptr &&
                        first_child->parent != nullptr && second_child->parent != nullptr &&
                        !bms_overlap(first_child->parent->relids, second_child->parent->relids);
                    Relids child_union =
                        owner_children_cover_parent
                            ? bms_union(first_child->parent->relids, second_child->parent->relids)
                            : nullptr;
                    owner_children_cover_parent =
                        owner_children_cover_parent &&
                        bms_equal(child_union, custom->path.parent->relids);
                    bms_free(child_union);
                } else if (owner_children_cover_parent) {
                    Path* child = reinterpret_cast<Path*>(linitial(custom->custom_paths));
                    owner_children_cover_parent =
                        child != nullptr && child->parent != nullptr &&
                        bms_equal(child->parent->relids, custom->path.parent->relids);
                }
                if (invalid_wrapper_abi || owner_recipe == nullptr || exact_owner_count != 1u ||
                    wrapper_mmsearch_count != 1u || owned_mmsearch == nullptr ||
                    !executable_shape || !owner_parent_contains_operands ||
                    !owner_children_cover_parent || custom->path.parallel_aware ||
                    custom->path.parallel_safe || custom->path.parallel_workers != 0 ||
                    !std::isfinite(custom->path.rows) || custom->path.rows < 0 ||
                    !std::isfinite(custom->path.startup_cost) || custom->path.startup_cost < 0 ||
                    !std::isfinite(custom->path.total_cost) ||
                    custom->path.total_cost < custom->path.startup_cost) {
                    reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_known_wrapper_abi);
                    return;
                }
                ++ownership.matches;
                for (Path* child : postgres_list<Path>(custom->custom_paths)) {
                    count_exact_parameterized_bmo_owners(
                        root, child, restriction, ownership, admit_known_wrapper_owner);
                }
                return;
            }
            bool invalid_wrapper_abi = false;
            if (known_join_owner &&
                per_row_known_path_owner_recipe(custom, invalid_wrapper_abi) == nullptr) {
                reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_known_wrapper_abi);
                return;
            }
            if (known_slice_wrapper && list_length(custom->custom_paths) != 1) {
                reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_known_wrapper_abi);
                return;
            }
            for (Path* child : postgres_list<Path>(custom->custom_paths)) {
                count_exact_parameterized_bmo_owners(
                    root, child, restriction, ownership, admit_known_wrapper_owner);
            }
            return;
        }
        if (IsA(path, NestPath) || IsA(path, MergePath) || IsA(path, HashPath)) {
            auto* join = reinterpret_cast<JoinPath*>(path);
            bool nested_owns_expected = false;
            for (RestrictInfo* nested : postgres_list<RestrictInfo>(join->joinrestrictinfo)) {
                if (admit_known_wrapper_owner && nested != nullptr &&
                    count_mmsearch_occurrences(reinterpret_cast<Node*>(nested->clause),
                                               get_cached_stub_oids()) != 0u) {
                    reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_nested_join_clause);
                    return;
                }
                nested_owns_expected =
                    nested_owns_expected || same_planner_restriction_identity(nested, restriction);
            }
            List* algorithm_clauses =
                IsA(path, MergePath)
                    ? reinterpret_cast<MergePath*>(path)->path_mergeclauses
                    : (IsA(path, HashPath) ? reinterpret_cast<HashPath*>(path)->path_hashclauses
                                           : NIL);
            if (admit_known_wrapper_owner) {
                for (RestrictInfo* algorithm_clause :
                     postgres_list<RestrictInfo>(algorithm_clauses)) {
                    if (algorithm_clause != nullptr &&
                        count_mmsearch_occurrences(
                            reinterpret_cast<Node*>(algorithm_clause->clause),
                            get_cached_stub_oids()) != 0u) {
                        reject_exact_parameterized_bmo_owner(ownership,
                                                             k_exact_bmo_nested_join_clause);
                        return;
                    }
                }
            }
            if (nested_owns_expected) {
                reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_nested_join_clause);
                return;
            }
            count_exact_parameterized_bmo_owners(
                root, join->outerjoinpath, restriction, ownership, admit_known_wrapper_owner);
            count_exact_parameterized_bmo_owners(
                root, join->innerjoinpath, restriction, ownership, admit_known_wrapper_owner);
            return;
        }
        Path* unary = nullptr;
        if (IsA(path, ProjectionPath)) {
            unary = reinterpret_cast<ProjectionPath*>(path)->subpath;
        } else if (IsA(path, ProjectSetPath)) {
            unary = reinterpret_cast<ProjectSetPath*>(path)->subpath;
        } else if (IsA(path, SortPath)) {
            unary = reinterpret_cast<SortPath*>(path)->subpath;
        } else if (IsA(path, IncrementalSortPath)) {
            unary = reinterpret_cast<IncrementalSortPath*>(path)->spath.subpath;
        } else if (IsA(path, MaterialPath)) {
            unary = reinterpret_cast<MaterialPath*>(path)->subpath;
        } else if (IsA(path, MemoizePath)) {
            unary = reinterpret_cast<MemoizePath*>(path)->subpath;
        } else if (IsA(path, UniquePath)) {
            unary = reinterpret_cast<UniquePath*>(path)->subpath;
        } else if (IsA(path, GatherPath)) {
            unary = reinterpret_cast<GatherPath*>(path)->subpath;
        } else if (IsA(path, GatherMergePath)) {
            unary = reinterpret_cast<GatherMergePath*>(path)->subpath;
        }
        if (unary != nullptr) {
            count_exact_parameterized_bmo_owners(
                root, unary, restriction, ownership, admit_known_wrapper_owner);
            return;
        }
        if (IsA(path, AppendPath)) {
            for (Path* child : postgres_list<Path>(reinterpret_cast<AppendPath*>(path)->subpaths)) {
                count_exact_parameterized_bmo_owners(
                    root, child, restriction, ownership, admit_known_wrapper_owner);
            }
            return;
        }
        if (IsA(path, MergeAppendPath)) {
            for (Path* child :
                 postgres_list<Path>(reinterpret_cast<MergeAppendPath*>(path)->subpaths)) {
                count_exact_parameterized_bmo_owners(
                    root, child, restriction, ownership, admit_known_wrapper_owner);
            }
            return;
        }
        if (IsA(path, GroupResultPath)) {
            /* GroupResultPath is childless, but its bare HAVING quals are executable
             * and are not represented by pathtarget or ParamPathInfo.  It can never
             * own a BMO restriction, so any surviving mmsearch here invalidates the
             * closed-world owner count instead of disappearing behind a leaf tag. */
            for (Node* qual :
                 postgres_list<Node>(reinterpret_cast<GroupResultPath*>(path)->quals)) {
                if (count_mmsearch_occurrences(qual, get_cached_stub_oids()) != 0u) {
                    reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_nested_join_clause);
                    return;
                }
            }
            return;
        }
        /* Closed-world ownership: these are the only childless core Path variants
         * admitted as leaves.  Any other tag may hide a child/owner layout this
         * narrow fallback does not understand. */
        if (IsA(path, Path) || IsA(path, IndexPath) || IsA(path, TidPath) ||
            IsA(path, TidRangePath))
            return;
        reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_unknown_core_path);
    }

    enum exact_parameterized_bmo_reconstruction_rejection : std::uint32_t {
        k_exact_bmo_reconstruct_invalid_input = 1u << 0,
        k_exact_bmo_reconstruct_operand_shape = 1u << 1,
        k_exact_bmo_reconstruct_target_rel = 1u << 2,
        k_exact_bmo_reconstruct_target_rte = 1u << 3,
        k_exact_bmo_reconstruct_required_outer = 1u << 4,
        k_exact_bmo_reconstruct_base_mmsearch = 1u << 5,
        k_exact_bmo_reconstruct_path_shape = 1u << 6,
        k_exact_bmo_reconstruct_owner_analysis = 1u << 7,
        k_exact_bmo_reconstruct_owner_multiplicity = 1u << 8,
        k_exact_bmo_reconstruct_ppi = 1u << 9,
        k_exact_bmo_reconstruct_owner_abi = 1u << 10,
        k_exact_bmo_reconstruct_residual_mmsearch = 1u << 11,
        k_exact_bmo_reconstruct_parallel = 1u << 12,
        k_exact_bmo_reconstruct_cost = 1u << 13,
    };

    enum exact_parameterized_bmo_reconstruction_abi_mismatch : std::uint32_t {
        k_exact_bmo_reconstruct_abi_invalid_tag = 1u << 0,
        k_exact_bmo_reconstruct_abi_missing_recipe = 1u << 1,
        k_exact_bmo_reconstruct_abi_recipe_target = 1u << 2,
        k_exact_bmo_reconstruct_abi_tag_multiplicity = 1u << 3,
        k_exact_bmo_reconstruct_abi_expression_multiplicity = 1u << 4,
        k_exact_bmo_reconstruct_abi_expression_shape = 1u << 5,
        k_exact_bmo_reconstruct_abi_expression_target = 1u << 6,
        k_exact_bmo_reconstruct_abi_dynamic_query = 1u << 7,
        k_exact_bmo_reconstruct_abi_dynamic_multiplicity = 1u << 8,
        k_exact_bmo_reconstruct_abi_constant_shape = 1u << 9,
        k_exact_bmo_reconstruct_abi_constant_multiplicity = 1u << 10,
        k_exact_bmo_reconstruct_abi_missing_dynamic = 1u << 11,
        k_exact_bmo_reconstruct_abi_missing_constant = 1u << 12,
        k_exact_bmo_reconstruct_abi_unexpected_constant = 1u << 13,
        k_exact_bmo_reconstruct_abi_incompatible_options = 1u << 14,
    };

    struct exact_parameterized_bmo_reconstruction_audit {
        std::uint32_t rejection_mask = 0u;
        std::uint32_t owner_abi_mismatch_mask = 0u;
        std::size_t expected_owner_count = 0u;
        std::size_t owner_tag_count = 0u;
        std::size_t owner_expression_count = 0u;
    };

    /* Core may discard a parameterized CustomPath before the higher join frontier
     * is formed.  Rebuild only the exact baserel recipe needed by that frontier;
     * AddBioseqColumnScanPath performs the normal selectivity/alignment costing
     * and does not add the result to RelOptInfo::pathlist.  The returned path is
     * private to the constructed NestPath below. */
    [[nodiscard]] Path*
    reconstruct_exact_parameterized_bmo_inner(PlannerInfo* root,
                                              RelOptInfo* target_input,
                                              RelOptInfo* provider_input,
                                              RestrictInfo* restriction,
                                              FuncExpr* mmsearch,
                                              exact_parameterized_bmo_reconstruction_audit& audit) {
        audit = {};
        auto reject =
            [&audit](
                const exact_parameterized_bmo_reconstruction_rejection reason) noexcept -> Path* {
            audit.rejection_mask |= static_cast<std::uint32_t>(reason);
            return nullptr;
        };
        if (root == nullptr || root->parse == nullptr || target_input == nullptr ||
            provider_input == nullptr || restriction == nullptr || mmsearch == nullptr ||
            list_length(mmsearch->args) < 2) {
            return reject(k_exact_bmo_reconstruct_invalid_input);
        }
        postgres_list<Node> operands(mmsearch->args);
        Var* target_var = try_cast_pg_node<Var>(operands.get_first());
        Var* query_var = try_cast_pg_node<Var>(operands.get_second());
        const bioseq_stub_oids& oids = get_cached_stub_oids();
        if (target_var == nullptr || query_var == nullptr ||
            !bioseq_is_concrete_rte_varno(target_var->varno) ||
            !bioseq_is_concrete_rte_varno(query_var->varno) || target_var->varlevelsup != 0 ||
            query_var->varlevelsup != 0 || target_var->varattno <= 0 || query_var->varattno <= 0 ||
            !bms_is_empty(target_var->varnullingrels) || !bms_is_empty(query_var->varnullingrels) ||
            target_var->vartype != oids.bioseq_seq_type_oid ||
            query_var->vartype != oids.bioseq_seq_type_oid ||
            !mmsearch_typmods_share_alphabet(target_var->vartypmod, query_var->vartypmod) ||
            target_var->varcollid != query_var->varcollid) {
            return reject(k_exact_bmo_reconstruct_operand_shape);
        }
        if (target_input->reloptkind != RELOPT_BASEREL ||
            target_input->relid != target_var->varno ||
            bms_num_members(target_input->relids) != 1 ||
            !bms_is_member(static_cast<int>(target_var->varno), target_input->relids) ||
            !bms_is_member(static_cast<int>(query_var->varno), provider_input->relids) ||
            bms_overlap(target_input->relids, provider_input->relids)) {
            return reject(k_exact_bmo_reconstruct_target_rel);
        }
        if (target_input->relid <= 0 || target_input->relid >= root->simple_rel_array_size ||
            root->simple_rel_array[target_input->relid] != target_input ||
            target_input->relid > static_cast<Index>(list_length(root->parse->rtable))) {
            return reject(k_exact_bmo_reconstruct_target_rel);
        }
        RangeTblEntry* target_rte = rt_fetch(target_input->relid, root->parse->rtable);
        if (target_rte == nullptr || target_rte->rtekind != RTE_RELATION ||
            target_rte->securityQuals != NIL || !OidIsValid(target_rte->relid) ||
            !relation_oid_uses_column_table_access_method(target_rte->relid)) {
            return reject(k_exact_bmo_reconstruct_target_rte);
        }

        postgres_list<RestrictInfo> base_mmsearch_restrictions;
        FuncExpr* base_mmsearch = nullptr;
        for (RestrictInfo* base : postgres_list<RestrictInfo>(target_input->baserestrictinfo)) {
            const std::size_t calls =
                base == nullptr
                    ? 0u
                    : count_mmsearch_occurrences(reinterpret_cast<Node*>(base->clause), oids);
            if (calls == 0u)
                continue;
            FuncExpr* expression =
                calls == 1u ? try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(base->clause))
                            : nullptr;
            if (expression == nullptr || !is_mmsearch_funcid(expression->funcid, oids) ||
                (expression->funcid == oids.mmsearch_oid && list_length(expression->args) != 2) ||
                (expression->funcid == oids.mmsearch_with_opts_oid &&
                 list_length(expression->args) != 3) ||
                base_mmsearch != nullptr) {
                return reject(k_exact_bmo_reconstruct_base_mmsearch);
            }
            postgres_list<Node> base_operands(expression->args);
            Const* constant_query = try_cast_pg_node<Const>(base_operands.get_second());
            if (!equal(base_operands.get_first(), target_var) || constant_query == nullptr ||
                constant_query->constisnull ||
                constant_query->consttype != oids.bioseq_seq_type_oid ||
                !mmsearch_typmods_share_alphabet(target_var->vartypmod,
                                                 constant_query->consttypmod)) {
                return reject(k_exact_bmo_reconstruct_base_mmsearch);
            }
            base_mmsearch = expression;
            base_mmsearch_restrictions.append(base);
        }
        if (base_mmsearch != nullptr) {
            if (query_var->varno > static_cast<Index>(list_length(root->parse->rtable))) {
                return reject(k_exact_bmo_reconstruct_base_mmsearch);
            }
            RangeTblEntry* query_rte = rt_fetch(query_var->varno, root->parse->rtable);
            FuncExpr* normalized_primary = copyObject(mmsearch);
            if (list_length(normalized_primary->args) == 3) {
                Node* option_expression =
                    reinterpret_cast<Node*>(list_nth(normalized_primary->args, 2));
                Node* folded = eval_const_expressions(
                    root, reinterpret_cast<Node*>(copyObject(option_expression)));
                lfirst(list_nth_cell(normalized_primary->args, 2)) = folded;
            }
            if (query_rte == nullptr || query_rte->rtekind != RTE_RELATION ||
                query_rte->securityQuals != NIL || !OidIsValid(query_rte->relid) ||
                relation_oid_uses_column_table_access_method(query_rte->relid) ||
                !bounded_batch_fallback_options_are_supported(
                    normalized_primary, base_mmsearch, oids)) {
                return reject(k_exact_bmo_reconstruct_base_mmsearch);
            }
        }

        Relids required_outer = bms_add_member(bms_copy(target_input->lateral_relids),
                                               static_cast<int>(query_var->varno));
        if (required_outer == nullptr || bms_overlap(required_outer, target_input->relids) ||
            !bms_is_subset(required_outer, provider_input->relids)) {
            bms_free(required_outer);
            return reject(k_exact_bmo_reconstruct_required_outer);
        }
        Path* candidate =
            add_bioseq_column_scan_path(root, target_input, target_rte, required_outer);
        /* get_baserel_parampathinfo may retain required_outer in the planner's PPI;
         * it therefore remains planner-owned after the constructor call. */
        auto* custom = candidate != nullptr && IsA(candidate, CustomPath)
                           ? reinterpret_cast<CustomPath*>(candidate)
                           : nullptr;
        if (custom == nullptr || custom->methods != bioseq_column_scan_path_methods() ||
            candidate->parent != target_input || candidate->pathtype != T_CustomScan ||
            list_length(custom->custom_private) != 3) {
            return reject(k_exact_bmo_reconstruct_path_shape);
        }
        exact_parameterized_bmo_ownership owner_analysis{};
        count_exact_parameterized_bmo_owners(root, candidate, restriction, owner_analysis);
        if (owner_analysis.invalid) {
            audit.rejection_mask |= owner_analysis.rejection_mask << 16;
            return reject(k_exact_bmo_reconstruct_owner_analysis);
        }
        if (owner_analysis.matches != 1u) {
            return reject(k_exact_bmo_reconstruct_owner_multiplicity);
        }
        const std::size_t expected_owner_count =
            1u + static_cast<std::size_t>(base_mmsearch_restrictions.size());
        audit.expected_owner_count = expected_owner_count;
        if (expected_owner_count > 2u ||
            list_length(custom->custom_restrictinfo) != static_cast<int>(expected_owner_count)) {
            return reject(k_exact_bmo_reconstruct_owner_multiplicity);
        }
        std::size_t dynamic_owner_count = 0u;
        std::size_t base_owner_count = 0u;
        for (RestrictInfo* owned : postgres_list<RestrictInfo>(custom->custom_restrictinfo)) {
            if (same_planner_restriction_identity(owned, restriction)) {
                ++dynamic_owner_count;
            } else if (list_member_ptr(base_mmsearch_restrictions.get(), owned)) {
                ++base_owner_count;
            } else {
                return reject(k_exact_bmo_reconstruct_owner_multiplicity);
            }
        }
        if (dynamic_owner_count != 1u ||
            base_owner_count != static_cast<std::size_t>(base_mmsearch_restrictions.size())) {
            return reject(k_exact_bmo_reconstruct_owner_multiplicity);
        }
        if (candidate->param_info == nullptr ||
            !bms_equal(PATH_REQ_OUTER(candidate), required_outer) ||
            !bms_is_member(static_cast<int>(query_var->varno), PATH_REQ_OUTER(candidate)) ||
            !bms_is_subset(PATH_REQ_OUTER(candidate), provider_input->relids)) {
            return reject(k_exact_bmo_reconstruct_ppi);
        }
        std::size_t ppi_dynamic_count = 0u;
        for (RestrictInfo* ppi_clause :
             postgres_list<RestrictInfo>(candidate->param_info->ppi_clauses)) {
            const std::size_t calls =
                ppi_clause == nullptr
                    ? 0u
                    : count_mmsearch_occurrences(reinterpret_cast<Node*>(ppi_clause->clause), oids);
            if (same_planner_restriction_identity(ppi_clause, restriction)) {
                ++ppi_dynamic_count;
                if (calls != 1u)
                    return reject(k_exact_bmo_reconstruct_ppi);
            } else if (calls != 0u) {
                return reject(k_exact_bmo_reconstruct_ppi);
            }
        }
        if (ppi_dynamic_count != 1u) {
            return reject(k_exact_bmo_reconstruct_ppi);
        }

        bool invalid_owner_abi = false;
        Node* owner_recipe = per_row_base_scan_owner_recipe(custom, invalid_owner_abi);
        List* owner_expressions = NIL;
        std::size_t owner_tag_count = 0u;
        for (Node* private_node : postgres_list<Node>(custom->custom_private)) {
            if (private_node == nullptr || !IsA(private_node, List))
                continue;
            List* tag = reinterpret_cast<List*>(private_node);
            Node* magic = list_length(tag) >= 1 ? reinterpret_cast<Node*>(linitial(tag)) : nullptr;
            if (magic == nullptr || !IsA(magic, Integer) ||
                intVal(magic) != k_bioseq_mmsearch_owner_magic)
                continue;
            ++owner_tag_count;
            Node* expressions =
                list_length(tag) == 3 ? reinterpret_cast<Node*>(list_nth(tag, 2)) : nullptr;
            if (list_length(tag) != 3 || !IsA(reinterpret_cast<Node*>(list_nth(tag, 1)), Integer) ||
                intVal(list_nth(tag, 1)) != k_bioseq_mmsearch_owner_version ||
                expressions == nullptr || !IsA(expressions, List)) {
                invalid_owner_abi = true;
            } else {
                owner_expressions = reinterpret_cast<List*>(expressions);
            }
        }
        audit.owner_tag_count = owner_tag_count;
        audit.owner_expression_count = static_cast<std::size_t>(list_length(owner_expressions));
        if (invalid_owner_abi) {
            audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_invalid_tag;
        }
        if (owner_recipe == nullptr) {
            audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_missing_recipe;
        } else if (!equal(owner_recipe, target_var)) {
            audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_recipe_target;
        }
        if (owner_tag_count != 1u) {
            audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_tag_multiplicity;
        }
        if (list_length(owner_expressions) != static_cast<int>(expected_owner_count)) {
            audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_expression_multiplicity;
        }
        if (audit.owner_abi_mismatch_mask != 0u) {
            return reject(k_exact_bmo_reconstruct_owner_abi);
        }
        FuncExpr* dynamic_expression = nullptr;
        FuncExpr* constant_expression = nullptr;
        for (int index = 0; index < static_cast<int>(expected_owner_count); ++index) {
            RestrictInfo* owned =
                reinterpret_cast<RestrictInfo*>(list_nth(custom->custom_restrictinfo, index));
            FuncExpr* expression = try_cast_pg_node<FuncExpr>(
                reinterpret_cast<Node*>(list_nth(owner_expressions, index)));
            if (expression == nullptr || list_length(expression->args) < 2) {
                audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_expression_shape;
                return reject(k_exact_bmo_reconstruct_owner_abi);
            }
            if (!equal(linitial(expression->args), target_var)) {
                audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_expression_target;
                return reject(k_exact_bmo_reconstruct_owner_abi);
            }
            if (same_planner_restriction_identity(owned, restriction)) {
                if (!equal(lsecond(expression->args), query_var)) {
                    audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_dynamic_query;
                    return reject(k_exact_bmo_reconstruct_owner_abi);
                }
                if (dynamic_expression != nullptr) {
                    audit.owner_abi_mismatch_mask |=
                        k_exact_bmo_reconstruct_abi_dynamic_multiplicity;
                    return reject(k_exact_bmo_reconstruct_owner_abi);
                }
                dynamic_expression = expression;
            } else {
                Const* constant =
                    try_cast_pg_node<Const>(reinterpret_cast<Node*>(lsecond(expression->args)));
                if (constant == nullptr || constant->constisnull) {
                    audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_constant_shape;
                    return reject(k_exact_bmo_reconstruct_owner_abi);
                }
                if (constant_expression != nullptr) {
                    audit.owner_abi_mismatch_mask |=
                        k_exact_bmo_reconstruct_abi_constant_multiplicity;
                    return reject(k_exact_bmo_reconstruct_owner_abi);
                }
                constant_expression = expression;
            }
        }
        if (dynamic_expression == nullptr) {
            audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_missing_dynamic;
        }
        if (base_mmsearch != nullptr && constant_expression == nullptr) {
            audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_missing_constant;
        }
        if (base_mmsearch == nullptr && constant_expression != nullptr) {
            audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_unexpected_constant;
        }
        if (base_mmsearch != nullptr && dynamic_expression != nullptr &&
            constant_expression != nullptr &&
            !bounded_batch_fallback_options_are_supported(
                dynamic_expression, constant_expression, oids)) {
            audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_incompatible_options;
        }
        if (audit.owner_abi_mismatch_mask != 0u) {
            return reject(k_exact_bmo_reconstruct_owner_abi);
        }
        for (int index = 0; index < 2; ++index) {
            Node* residual_node = reinterpret_cast<Node*>(list_nth(custom->custom_private, index));
            if (residual_node != nullptr && !IsA(residual_node, List)) {
                return reject(k_exact_bmo_reconstruct_owner_abi);
            }
            for (RestrictInfo* residual : postgres_list<RestrictInfo>(
                     residual_node == nullptr ? NIL : reinterpret_cast<List*>(residual_node))) {
                if (residual != nullptr &&
                    count_mmsearch_occurrences(reinterpret_cast<Node*>(residual->clause), oids) !=
                        0u) {
                    return reject(k_exact_bmo_reconstruct_residual_mmsearch);
                }
            }
        }
        if (candidate->parallel_aware || candidate->parallel_safe ||
            candidate->parallel_workers != 0) {
            return reject(k_exact_bmo_reconstruct_parallel);
        }
        if (!std::isfinite(candidate->rows) || candidate->rows < 0 ||
            !std::isfinite(candidate->startup_cost) || candidate->startup_cost < 0 ||
            !std::isfinite(candidate->total_cost) ||
            candidate->total_cost < candidate->startup_cost) {
            return reject(k_exact_bmo_reconstruct_cost);
        }
        return candidate;
    }

    struct exact_parameterized_bmo_retainer_audit {
        std::size_t candidates = 0u;
        std::size_t nest_paths = 0u;
        std::size_t inner_nest_paths = 0u;
        std::size_t root_owned_clause_paths = 0u;
        std::size_t root_foreign_mmsearch_paths = 0u;
        std::size_t outer_matches = 0u;
        std::size_t inner_matches = 0u;
        std::size_t retained_paths = 0u;
        std::size_t available_bmo_paths = 0u;
        std::size_t compatible_bmo_paths = 0u;
        std::size_t available_outer_paths = 0u;
        std::size_t constructed_paths = 0u;
        std::size_t reconstructed_bmo_attempts = 0u;
        std::size_t reconstructed_bmo_paths = 0u;
        std::size_t target_var_resolved = 0u;
        std::size_t query_var_resolved = 0u;
        std::uint32_t target_relid_side_mask = 0u;
        std::uint32_t target_base_side_mask = 0u;
        std::uint32_t query_relid_side_mask = 0u;
        std::size_t provider_has_query = 0u;
        std::size_t target_paths = 0u;
        std::size_t target_custom_paths = 0u;
        std::size_t target_bioseq_paths = 0u;
        std::size_t target_bmo_tags = 0u;
        std::size_t target_custom_restrictions = 0u;
        std::size_t target_custom_pointer_matches = 0u;
        std::size_t target_custom_serial_matches = 0u;
        std::size_t target_custom_clause_matches = 0u;
        std::size_t target_custom_exact_matches = 0u;
        std::size_t target_ppi_paths = 0u;
        std::size_t target_ppi_clauses = 0u;
        std::size_t target_ppi_serial_matches = 0u;
        std::size_t target_ppi_clause_matches = 0u;
        std::size_t target_ppi_exact_matches = 0u;
        std::uint32_t outer_rejection_mask = 0u;
        std::uint32_t inner_rejection_mask = 0u;
        std::uint32_t target_owner_rejection_mask = 0u;
        std::uint32_t target_abi_mismatch_mask = 0u;
        std::uint32_t target_ppi_mismatch_mask = 0u;
        std::uint32_t target_validation_mismatch_mask = 0u;
        std::uint32_t reconstructed_bmo_rejection_mask = 0u;
        std::uint32_t reconstructed_owner_abi_mismatch_mask = 0u;
        std::size_t reconstructed_expected_owner_count = 0u;
        std::size_t reconstructed_owner_tag_count = 0u;
        std::size_t reconstructed_owner_expression_count = 0u;
        std::uint64_t joinrel_relids_low = 0u;
        std::uint64_t left_relids_low = 0u;
        std::uint64_t right_relids_low = 0u;
        std::size_t left_relid_count = 0u;
        std::size_t right_relid_count = 0u;
        std::uint32_t wrapper_side_mask = 0u;
        std::size_t wrapper_owns_expected = 0u;
        std::uint32_t wrapper_method_mask = 0u;
        std::size_t wrapper_version = 0u;
        std::size_t wrapper_restriction_count = 0u;
        std::size_t wrapper_child_count = 0u;
        std::size_t wrapper_pointer_matches = 0u;
        std::size_t wrapper_serial_matches = 0u;
        std::size_t wrapper_clause_matches = 0u;
        std::size_t wrapper_expected_serial = 0u;
        std::size_t wrapper_owned_serial = 0u;
        std::size_t wrapper_expected_target_varno = 0u;
        std::size_t wrapper_expected_query_varno = 0u;
        std::size_t wrapper_private_target_varno = 0u;
        std::size_t wrapper_private_query_varno = 0u;
        std::size_t wrapper_private_target_equal = 0u;
        std::size_t wrapper_private_query_equal = 0u;
        std::size_t wrapper_owner0_target_varno = 0u;
        std::size_t wrapper_owner0_query_varno = 0u;
        std::size_t wrapper_owner1_target_varno = 0u;
        std::size_t wrapper_owner1_query_varno = 0u;
        std::uint64_t wrapper_parent_relids_low = 0u;
        std::uint64_t wrapper_child0_relids_low = 0u;
        std::uint64_t wrapper_child1_relids_low = 0u;
        std::size_t current_pair_paths = 0u;
        std::size_t foreign_decomposition_paths = 0u;
        std::size_t safe_foreign_paths = 0u;
        std::size_t foreign_owner_matches = 0u;
        std::uint32_t foreign_rejection_mask = 0u;
        std::uint32_t foreign_owner_rejection_mask = 0u;
    };

    enum exact_parameterized_bmo_retainer_abi_mismatch : std::uint32_t {
        k_exact_bmo_retainer_invalid_owner_tag = 1u << 0,
        k_exact_bmo_retainer_missing_custom_restriction = 1u << 1,
        k_exact_bmo_retainer_missing_custom_serial = 1u << 2,
        k_exact_bmo_retainer_missing_custom_clause = 1u << 3,
        k_exact_bmo_retainer_missing_custom_identity = 1u << 4,
        k_exact_bmo_retainer_multiple_custom_identities = 1u << 5,
    };

    enum exact_parameterized_bmo_retainer_ppi_mismatch : std::uint32_t {
        k_exact_bmo_retainer_missing_param_info = 1u << 0,
        k_exact_bmo_retainer_empty_ppi_clauses = 1u << 1,
        k_exact_bmo_retainer_missing_ppi_serial = 1u << 2,
        k_exact_bmo_retainer_missing_ppi_clause = 1u << 3,
        k_exact_bmo_retainer_missing_ppi_identity = 1u << 4,
        k_exact_bmo_retainer_multiple_ppi_identities = 1u << 5,
    };

    enum exact_parameterized_bmo_retainer_validation_mismatch : std::uint32_t {
        k_exact_bmo_retainer_candidate_parent = 1u << 0,
        k_exact_bmo_retainer_candidate_owner_analysis = 1u << 1,
        k_exact_bmo_retainer_candidate_owner_multiplicity = 1u << 2,
        k_exact_bmo_retainer_candidate_unparameterized = 1u << 3,
        k_exact_bmo_retainer_candidate_missing_query_relid = 1u << 4,
        k_exact_bmo_retainer_candidate_foreign_required_outer = 1u << 5,
        k_exact_bmo_retainer_candidate_parallel = 1u << 6,
        k_exact_bmo_retainer_candidate_nonfinite_cost = 1u << 7,
        k_exact_bmo_retainer_candidate_negative_cost = 1u << 8,
    };

    [[nodiscard]] bool retain_join_paths_with_custom_restriction_owner(
        PlannerInfo* root,
        RelOptInfo* joinrel,
        RelOptInfo* left_input,
        RelOptInfo* right_input,
        JoinPathExtraData* extra,
        RestrictInfo* restriction,
        FuncExpr* mmsearch,
        exact_parameterized_bmo_retainer_audit* audit = nullptr) {
        if (root == nullptr || joinrel == nullptr || left_input == nullptr ||
            right_input == nullptr || extra == nullptr || restriction == nullptr ||
            mmsearch == nullptr || list_length(mmsearch->args) < 2)
            return false;
        exact_parameterized_bmo_retainer_audit local_audit{};
        local_audit.joinrel_relids_low = exact_bmo_audit_relids_low(joinrel->relids);
        local_audit.left_relids_low = exact_bmo_audit_relids_low(left_input->relids);
        local_audit.right_relids_low = exact_bmo_audit_relids_low(right_input->relids);
        local_audit.left_relid_count =
            static_cast<std::size_t>(bms_num_members(left_input->relids));
        local_audit.right_relid_count =
            static_cast<std::size_t>(bms_num_members(right_input->relids));
        postgres_list<Path> retained;
        for (Path* path : postgres_list<Path>(joinrel->pathlist)) {
            ++local_audit.candidates;
            if (path == nullptr || !IsA(path, NestPath))
                continue;
            ++local_audit.nest_paths;
            auto* nest = reinterpret_cast<NestPath*>(path);
            if (nest->jpath.jointype != JOIN_INNER)
                continue;
            ++local_audit.inner_nest_paths;
            RelOptInfo* outer_parent =
                nest->jpath.outerjoinpath == nullptr ? nullptr : nest->jpath.outerjoinpath->parent;
            RelOptInfo* inner_parent =
                nest->jpath.innerjoinpath == nullptr ? nullptr : nest->jpath.innerjoinpath->parent;
            const bool current_direct = outer_parent != nullptr && inner_parent != nullptr &&
                                        bms_equal(outer_parent->relids, left_input->relids) &&
                                        bms_equal(inner_parent->relids, right_input->relids);
            const bool current_swapped = outer_parent != nullptr && inner_parent != nullptr &&
                                         bms_equal(outer_parent->relids, right_input->relids) &&
                                         bms_equal(inner_parent->relids, left_input->relids);
            const bool current_pair = current_direct || current_swapped;
            if (current_pair) {
                ++local_audit.current_pair_paths;
            } else {
                ++local_audit.foreign_decomposition_paths;
            }

            std::size_t exact_root_copies = 0u;
            bool foreign_root_mmsearch = false;
            for (RestrictInfo* root_clause :
                 postgres_list<RestrictInfo>(nest->jpath.joinrestrictinfo)) {
                const std::size_t calls =
                    root_clause == nullptr
                        ? 0u
                        : count_mmsearch_occurrences(reinterpret_cast<Node*>(root_clause->clause),
                                                     get_cached_stub_oids());
                if (same_planner_restriction_identity(root_clause, restriction)) {
                    ++exact_root_copies;
                    if (calls != 1u)
                        foreign_root_mmsearch = true;
                } else if (calls != 0u) {
                    /* A copied/equivalent rendezvous clause cannot be removed by the
                     * exact pointer identity, and would remain an executable SQL stub. */
                    foreign_root_mmsearch = true;
                }
            }
            if (exact_root_copies > 1u || foreign_root_mmsearch) {
                ++local_audit.root_foreign_mmsearch_paths;
                continue;
            }
            if (exact_root_copies == 1u) {
                ++local_audit.root_owned_clause_paths;
            }
            if (!current_pair) {
                bool exact_foreign_partition =
                    path->parent == joinrel && outer_parent != nullptr && inner_parent != nullptr &&
                    !bms_overlap(outer_parent->relids, inner_parent->relids);
                Relids foreign_union = exact_foreign_partition
                                           ? bms_union(outer_parent->relids, inner_parent->relids)
                                           : nullptr;
                exact_foreign_partition =
                    exact_foreign_partition && bms_equal(foreign_union, joinrel->relids);
                bms_free(foreign_union);
                if (!exact_foreign_partition) {
                    local_audit.foreign_rejection_mask |= 1u << 0;
                    continue;
                }
                if (exact_root_copies != 0u || foreign_root_mmsearch) {
                    local_audit.foreign_rejection_mask |= 1u << 1;
                    continue;
                }
                if (batch_join_path_contains_parallel_execution(path) ||
                    !std::isfinite(path->startup_cost) || !std::isfinite(path->total_cost) ||
                    path->startup_cost < 0 || path->total_cost < path->startup_cost) {
                    local_audit.foreign_rejection_mask |= 1u << 2;
                    continue;
                }
                exact_parameterized_bmo_ownership foreign_owner{};
                count_exact_parameterized_bmo_owners(root,
                                                     path,
                                                     restriction,
                                                     foreign_owner,
                                                     /*admit_known_wrapper_owner=*/true);
                local_audit.foreign_owner_matches += foreign_owner.matches;
                local_audit.foreign_owner_rejection_mask |= foreign_owner.rejection_mask;
                if (foreign_owner.invalid) {
                    local_audit.foreign_rejection_mask |= 1u << 3;
                    continue;
                }
                if (foreign_owner.matches != 1u) {
                    local_audit.foreign_rejection_mask |= 1u << 4;
                    continue;
                }
                retained.append(path);
                ++local_audit.safe_foreign_paths;
                continue;
            }

            exact_parameterized_bmo_ownership outer{};
            exact_parameterized_bmo_ownership inner{};
            count_exact_parameterized_bmo_owners(
                root, nest->jpath.outerjoinpath, restriction, outer);
            count_exact_parameterized_bmo_owners(
                root, nest->jpath.innerjoinpath, restriction, inner);
            local_audit.outer_matches += outer.matches;
            local_audit.inner_matches += inner.matches;
            local_audit.outer_rejection_mask |= outer.rejection_mask;
            local_audit.inner_rejection_mask |= inner.rejection_mask;
            auto capture_wrapper_audit = [&local_audit](
                                             exact_parameterized_bmo_ownership const& ownership,
                                             const std::uint32_t side) {
                if (ownership.wrapper_owns_expected == 0u)
                    return;
                local_audit.wrapper_side_mask |= side;
                local_audit.wrapper_owns_expected += ownership.wrapper_owns_expected;
                local_audit.wrapper_method_mask |= ownership.wrapper_method_mask;
                if (local_audit.wrapper_expected_serial == 0u) {
                    local_audit.wrapper_version = ownership.wrapper_version;
                    local_audit.wrapper_restriction_count = ownership.wrapper_restriction_count;
                    local_audit.wrapper_child_count = ownership.wrapper_child_count;
                    local_audit.wrapper_pointer_matches = ownership.wrapper_pointer_matches;
                    local_audit.wrapper_serial_matches = ownership.wrapper_serial_matches;
                    local_audit.wrapper_clause_matches = ownership.wrapper_clause_matches;
                    local_audit.wrapper_expected_serial = ownership.expected_rinfo_serial;
                    local_audit.wrapper_owned_serial = ownership.wrapper_rinfo_serial;
                    local_audit.wrapper_expected_target_varno = ownership.expected_target_varno;
                    local_audit.wrapper_expected_query_varno = ownership.expected_query_varno;
                    local_audit.wrapper_private_target_varno =
                        ownership.wrapper_private_target_varno;
                    local_audit.wrapper_private_query_varno = ownership.wrapper_private_query_varno;
                    local_audit.wrapper_private_target_equal =
                        ownership.wrapper_private_target_equal;
                    local_audit.wrapper_private_query_equal = ownership.wrapper_private_query_equal;
                    local_audit.wrapper_owner0_target_varno = ownership.wrapper_owner0_target_varno;
                    local_audit.wrapper_owner0_query_varno = ownership.wrapper_owner0_query_varno;
                    local_audit.wrapper_owner1_target_varno = ownership.wrapper_owner1_target_varno;
                    local_audit.wrapper_owner1_query_varno = ownership.wrapper_owner1_query_varno;
                    local_audit.wrapper_parent_relids_low = ownership.wrapper_parent_relids_low;
                    local_audit.wrapper_child0_relids_low = ownership.wrapper_child0_relids_low;
                    local_audit.wrapper_child1_relids_low = ownership.wrapper_child1_relids_low;
                }
            };
            capture_wrapper_audit(outer, 1u);
            capture_wrapper_audit(inner, 2u);
            if (outer.invalid || inner.invalid || outer.matches != 0u || inner.matches != 1u)
                continue;

            if (exact_root_copies == 0u) {
                retained.append(path);
            } else {
                /* Core retains the parameter clause in JoinPath::joinrestrictinfo even
                 * when the parameterized BMO inner path owns it.  Clone only the root
                 * NestPath and remove that exact pointer so create_nestloop_plan cannot
                 * emit the rendezvous SQL function as a Join Filter.  Costs stay
                 * conservative because the original qual charge is retained. */
                auto* clone = makeNode(NestPath);
                *clone = *nest;
                postgres_list<RestrictInfo> residual;
                for (RestrictInfo* root_clause :
                     postgres_list<RestrictInfo>(nest->jpath.joinrestrictinfo)) {
                    if (!same_planner_restriction_identity(root_clause, restriction))
                        residual.append(root_clause);
                }
                clone->jpath.joinrestrictinfo = residual.release();
                retained.append(reinterpret_cast<Path*>(clone));
            }
        }
        /* Core does not necessarily form a NestPath from a custom parameterized
         * baserel path.  If no incumbent validates ownership, construct that exact
         * normal-cost shape from the target input's BMO path and the other input's
         * cheapest compatible path. */
        if (retained.is_empty()) {
            postgres_list<Node> operands(mmsearch->args);
            Var* target_var = try_cast_pg_node<Var>(operands.get_first());
            Var* query_var = try_cast_pg_node<Var>(operands.get_second());
            local_audit.target_var_resolved = target_var == nullptr ? 0u : 1u;
            local_audit.query_var_resolved = query_var == nullptr ? 0u : 1u;
            auto relid_side_mask = [left_input, right_input](Var* var) noexcept {
                std::uint32_t mask = 0u;
                if (var == nullptr || !bioseq_is_concrete_rte_varno(var->varno)) {
                    return mask;
                }
                if (bms_is_member(static_cast<int>(var->varno), left_input->relids)) {
                    mask |= 1u;
                }
                if (bms_is_member(static_cast<int>(var->varno), right_input->relids)) {
                    mask |= 2u;
                }
                return mask;
            };
            local_audit.target_relid_side_mask = relid_side_mask(target_var);
            local_audit.query_relid_side_mask = relid_side_mask(query_var);
            RelOptInfo* target_input = nullptr;
            RelOptInfo* provider_input = nullptr;
            auto input_is_target_base = [target_var](RelOptInfo* input) noexcept {
                return input != nullptr && target_var != nullptr &&
                       (input->reloptkind == RELOPT_BASEREL ||
                        input->reloptkind == RELOPT_OTHER_MEMBER_REL) &&
                       input->relid == target_var->varno;
            };
            if (input_is_target_base(left_input)) {
                local_audit.target_base_side_mask |= 1u;
                target_input = left_input;
                provider_input = right_input;
            } else if (input_is_target_base(right_input)) {
                local_audit.target_base_side_mask |= 2u;
                target_input = right_input;
                provider_input = left_input;
            }
            const bool provider_has_query =
                provider_input != nullptr && query_var != nullptr &&
                bioseq_is_concrete_rte_varno(query_var->varno) &&
                bms_is_member(static_cast<int>(query_var->varno), provider_input->relids);
            local_audit.provider_has_query = provider_has_query ? 1u : 0u;
            Path* best_bmo = nullptr;
            if (target_input != nullptr) {
                for (Path* candidate : postgres_list<Path>(target_input->pathlist)) {
                    ++local_audit.target_paths;
                    auto* custom = candidate != nullptr && IsA(candidate, CustomPath)
                                       ? reinterpret_cast<CustomPath*>(candidate)
                                       : nullptr;
                    if (custom != nullptr)
                        ++local_audit.target_custom_paths;
                    const bool bioseq_path =
                        custom != nullptr && custom->methods == bioseq_column_scan_path_methods();
                    if (bioseq_path)
                        ++local_audit.target_bioseq_paths;
                    std::size_t candidate_bmo_tags = 0u;
                    if (bioseq_path) {
                        for (Node* private_node : postgres_list<Node>(custom->custom_private)) {
                            if (private_node == nullptr || !IsA(private_node, List)) {
                                continue;
                            }
                            List* tag = reinterpret_cast<List*>(private_node);
                            Node* magic = list_length(tag) >= 1
                                              ? reinterpret_cast<Node*>(linitial(tag))
                                              : nullptr;
                            if (magic != nullptr && IsA(magic, Integer) &&
                                intVal(magic) == k_bioseq_mmsearch_owner_magic) {
                                ++candidate_bmo_tags;
                                ++local_audit.target_bmo_tags;
                            }
                        }
                    }
                    if (candidate_bmo_tags != 0u) {
                        bool invalid_owner_abi = false;
                        Node* recipe = per_row_base_scan_owner_recipe(custom, invalid_owner_abi);
                        if (candidate_bmo_tags != 1u || invalid_owner_abi || recipe == nullptr) {
                            local_audit.target_abi_mismatch_mask |=
                                k_exact_bmo_retainer_invalid_owner_tag;
                        }
                        std::size_t candidate_pointer_matches = 0u;
                        std::size_t candidate_serial_matches = 0u;
                        std::size_t candidate_clause_matches = 0u;
                        std::size_t candidate_exact_matches = 0u;
                        for (RestrictInfo* owned :
                             postgres_list<RestrictInfo>(custom->custom_restrictinfo)) {
                            ++local_audit.target_custom_restrictions;
                            if (owned == restriction) {
                                ++candidate_pointer_matches;
                                ++local_audit.target_custom_pointer_matches;
                            }
                            if (owned != nullptr && restriction != nullptr &&
                                owned->rinfo_serial > 0 &&
                                owned->rinfo_serial == restriction->rinfo_serial) {
                                ++candidate_serial_matches;
                                ++local_audit.target_custom_serial_matches;
                            }
                            if (owned != nullptr && restriction != nullptr &&
                                equal(owned->clause, restriction->clause)) {
                                ++candidate_clause_matches;
                                ++local_audit.target_custom_clause_matches;
                            }
                            if (same_planner_restriction_identity(owned, restriction)) {
                                ++candidate_exact_matches;
                                ++local_audit.target_custom_exact_matches;
                            }
                        }
                        if (custom->custom_restrictinfo == NIL) {
                            local_audit.target_abi_mismatch_mask |=
                                k_exact_bmo_retainer_missing_custom_restriction;
                        }
                        if (candidate_serial_matches == 0u) {
                            local_audit.target_abi_mismatch_mask |=
                                k_exact_bmo_retainer_missing_custom_serial;
                        }
                        if (candidate_clause_matches == 0u) {
                            local_audit.target_abi_mismatch_mask |=
                                k_exact_bmo_retainer_missing_custom_clause;
                        }
                        if (candidate_exact_matches == 0u) {
                            local_audit.target_abi_mismatch_mask |=
                                k_exact_bmo_retainer_missing_custom_identity;
                        } else if (candidate_exact_matches > 1u) {
                            local_audit.target_abi_mismatch_mask |=
                                k_exact_bmo_retainer_multiple_custom_identities;
                        }
                        if (candidate->param_info == nullptr) {
                            local_audit.target_ppi_mismatch_mask |=
                                k_exact_bmo_retainer_missing_param_info;
                        } else {
                            ++local_audit.target_ppi_paths;
                            std::size_t candidate_ppi_serial_matches = 0u;
                            std::size_t candidate_ppi_clause_matches = 0u;
                            std::size_t candidate_ppi_exact_matches = 0u;
                            for (RestrictInfo* parameter_clause :
                                 postgres_list<RestrictInfo>(candidate->param_info->ppi_clauses)) {
                                ++local_audit.target_ppi_clauses;
                                if (parameter_clause != nullptr && restriction != nullptr &&
                                    parameter_clause->rinfo_serial > 0 &&
                                    parameter_clause->rinfo_serial == restriction->rinfo_serial) {
                                    ++candidate_ppi_serial_matches;
                                    ++local_audit.target_ppi_serial_matches;
                                }
                                if (parameter_clause != nullptr && restriction != nullptr &&
                                    equal(parameter_clause->clause, restriction->clause)) {
                                    ++candidate_ppi_clause_matches;
                                    ++local_audit.target_ppi_clause_matches;
                                }
                                if (same_planner_restriction_identity(parameter_clause,
                                                                      restriction)) {
                                    ++candidate_ppi_exact_matches;
                                    ++local_audit.target_ppi_exact_matches;
                                }
                            }
                            if (candidate->param_info->ppi_clauses == NIL) {
                                local_audit.target_ppi_mismatch_mask |=
                                    k_exact_bmo_retainer_empty_ppi_clauses;
                            }
                            if (candidate_ppi_serial_matches == 0u) {
                                local_audit.target_ppi_mismatch_mask |=
                                    k_exact_bmo_retainer_missing_ppi_serial;
                            }
                            if (candidate_ppi_clause_matches == 0u) {
                                local_audit.target_ppi_mismatch_mask |=
                                    k_exact_bmo_retainer_missing_ppi_clause;
                            }
                            if (candidate_ppi_exact_matches == 0u) {
                                local_audit.target_ppi_mismatch_mask |=
                                    k_exact_bmo_retainer_missing_ppi_identity;
                            } else if (candidate_ppi_exact_matches > 1u) {
                                local_audit.target_ppi_mismatch_mask |=
                                    k_exact_bmo_retainer_multiple_ppi_identities;
                            }
                        }
                        (void)candidate_pointer_matches;
                    }
                    exact_parameterized_bmo_ownership candidate_analysis{};
                    count_exact_parameterized_bmo_owners(
                        root, candidate, restriction, candidate_analysis);
                    local_audit.target_owner_rejection_mask |= candidate_analysis.rejection_mask;
                    if (provider_has_query && candidate_analysis.matches != 0u) {
                        ++local_audit.available_bmo_paths;
                    }
                    Relids required = candidate == nullptr ? nullptr : PATH_REQ_OUTER(candidate);
                    if (candidate_bmo_tags != 0u || candidate_analysis.matches != 0u) {
                        if (candidate == nullptr || candidate->parent != target_input) {
                            local_audit.target_validation_mismatch_mask |=
                                k_exact_bmo_retainer_candidate_parent;
                        }
                        if (candidate_analysis.invalid) {
                            local_audit.target_validation_mismatch_mask |=
                                k_exact_bmo_retainer_candidate_owner_analysis;
                        }
                        if (candidate_analysis.matches != 1u) {
                            local_audit.target_validation_mismatch_mask |=
                                k_exact_bmo_retainer_candidate_owner_multiplicity;
                        }
                        if (required == nullptr) {
                            local_audit.target_validation_mismatch_mask |=
                                k_exact_bmo_retainer_candidate_unparameterized;
                        } else if (provider_has_query) {
                            if (!bms_is_member(static_cast<int>(query_var->varno), required)) {
                                local_audit.target_validation_mismatch_mask |=
                                    k_exact_bmo_retainer_candidate_missing_query_relid;
                            }
                            if (!bms_is_subset(required, provider_input->relids)) {
                                local_audit.target_validation_mismatch_mask |=
                                    k_exact_bmo_retainer_candidate_foreign_required_outer;
                            }
                        }
                        if (candidate != nullptr &&
                            (candidate->parallel_aware || candidate->parallel_workers != 0)) {
                            local_audit.target_validation_mismatch_mask |=
                                k_exact_bmo_retainer_candidate_parallel;
                        }
                        if (candidate != nullptr && !std::isfinite(candidate->total_cost)) {
                            local_audit.target_validation_mismatch_mask |=
                                k_exact_bmo_retainer_candidate_nonfinite_cost;
                        }
                        if (candidate != nullptr && candidate->total_cost < 0) {
                            local_audit.target_validation_mismatch_mask |=
                                k_exact_bmo_retainer_candidate_negative_cost;
                        }
                    }
                    if (!provider_has_query || candidate == nullptr ||
                        candidate->parent != target_input || candidate_analysis.invalid ||
                        candidate_analysis.matches != 1u || required == nullptr ||
                        !bms_is_member(static_cast<int>(query_var->varno), required) ||
                        !bms_is_subset(required, provider_input->relids) ||
                        candidate->parallel_aware || candidate->parallel_workers != 0 ||
                        !std::isfinite(candidate->total_cost) || candidate->total_cost < 0)
                        continue;
                    ++local_audit.compatible_bmo_paths;
                    if (best_bmo == nullptr || candidate->total_cost < best_bmo->total_cost) {
                        best_bmo = candidate;
                    }
                }
                if (best_bmo == nullptr && provider_has_query) {
                    ++local_audit.reconstructed_bmo_attempts;
                    exact_parameterized_bmo_reconstruction_audit reconstruction_audit{};
                    best_bmo = reconstruct_exact_parameterized_bmo_inner(root,
                                                                         target_input,
                                                                         provider_input,
                                                                         restriction,
                                                                         mmsearch,
                                                                         reconstruction_audit);
                    local_audit.reconstructed_bmo_rejection_mask |=
                        reconstruction_audit.rejection_mask;
                    local_audit.reconstructed_owner_abi_mismatch_mask |=
                        reconstruction_audit.owner_abi_mismatch_mask;
                    local_audit.reconstructed_expected_owner_count =
                        reconstruction_audit.expected_owner_count;
                    local_audit.reconstructed_owner_tag_count =
                        reconstruction_audit.owner_tag_count;
                    local_audit.reconstructed_owner_expression_count =
                        reconstruction_audit.owner_expression_count;
                    if (best_bmo != nullptr) {
                        ++local_audit.reconstructed_bmo_paths;
                        ++local_audit.available_bmo_paths;
                        ++local_audit.compatible_bmo_paths;
                    }
                }
            }

            Path* best_outer = nullptr;
            if (provider_input != nullptr && best_bmo != nullptr) {
                for (Path* candidate : postgres_list<Path>(provider_input->pathlist)) {
                    if (candidate == nullptr || candidate->parent != provider_input ||
                        candidate->parallel_aware || candidate->parallel_workers != 0 ||
                        bms_overlap(PATH_REQ_OUTER(candidate), target_input->relids) ||
                        !std::isfinite(candidate->total_cost) || candidate->total_cost < 0)
                        continue;
                    ++local_audit.available_outer_paths;
                    if (best_outer == nullptr || candidate->total_cost < best_outer->total_cost) {
                        best_outer = candidate;
                    }
                }
            }
            if (best_outer != nullptr && best_bmo != nullptr) {
                Relids available_to_inner =
                    bms_union(provider_input->relids, PATH_REQ_OUTER(best_outer));
                const bool inner_requirements_available =
                    bms_is_subset(PATH_REQ_OUTER(best_bmo), available_to_inner);
                bms_free(available_to_inner);
                postgres_list<RestrictInfo> residual_join_clauses;
                bool residual_mmsearch = false;
                for (RestrictInfo* clause : postgres_list<RestrictInfo>(extra->restrictlist)) {
                    if (same_planner_restriction_identity(clause, restriction))
                        continue;
                    if (clause != nullptr &&
                        count_mmsearch_occurrences(reinterpret_cast<Node*>(clause->clause),
                                                   get_cached_stub_oids()) != 0u) {
                        residual_mmsearch = true;
                        break;
                    }
                    residual_join_clauses.append(clause);
                }
                if (inner_requirements_available && !residual_mmsearch) {
                    Relids required_outer = calc_nestloop_required_outer(provider_input->relids,
                                                                         PATH_REQ_OUTER(best_outer),
                                                                         target_input->relids,
                                                                         PATH_REQ_OUTER(best_bmo));
                    if (bms_overlap(required_outer, joinrel->relids)) {
                        bms_free(required_outer);
                    } else {
                        JoinCostWorkspace workspace{};
                        initial_cost_nestloop(
                            root, &workspace, JOIN_INNER, best_outer, best_bmo, extra);
                        NestPath* constructed = create_nestloop_path(
                            root,
                            joinrel,
                            JOIN_INNER,
                            &workspace,
                            extra,
                            best_outer,
                            best_bmo,
                            residual_join_clauses.get(),
                            build_join_pathkeys(root, joinrel, JOIN_INNER, best_outer->pathkeys),
                            required_outer);
                        bool constructed_has_mmsearch = false;
                        for (RestrictInfo* clause : postgres_list<RestrictInfo>(
                                 constructed == nullptr ? NIL
                                                        : constructed->jpath.joinrestrictinfo)) {
                            if (clause != nullptr &&
                                count_mmsearch_occurrences(reinterpret_cast<Node*>(clause->clause),
                                                           get_cached_stub_oids()) != 0u) {
                                constructed_has_mmsearch = true;
                                break;
                            }
                        }
                        if (constructed != nullptr && !constructed_has_mmsearch) {
                            retained.append(reinterpret_cast<Path*>(constructed));
                            ++local_audit.constructed_paths;
                        } else if (constructed == nullptr) {
                            bms_free(required_outer);
                        }
                    }
                }
            }
        }
        local_audit.retained_paths = static_cast<std::size_t>(retained.size());
        if (audit != nullptr)
            *audit = local_audit;
        if (retained.is_empty())
            return false;
        joinrel->pathlist = retained.release();
        joinrel->partial_pathlist = NIL;
        return true;
    }

} // export custom-scan partition declarations
