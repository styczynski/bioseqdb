/**
 * @file
 * @brief Owns target projection, helper lineage, and carrier path discovery.
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

export module bioseq.postgres.table:custom_scan_join_ownership_carrier_lineage;

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

    /** @brief Tracks forbidden target references while validating a locator projection. */
    struct batch_join_target_projection_probe {
        Index target_varno = 0;
        AttrNumber target_seq_attno = InvalidAttrNumber;
        const bioseq_stub_oids* oids = nullptr;
        bool allow_mmsearch_qualifier = false;
        bool forbidden = false;
    };

    /** @brief Finds target sequence Vars that would escape into ordinary SQL evaluation. */
    struct batch_join_target_projection_visitor {
        /** @brief Examines one projection node while treating owned helpers as atomic. */
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

    /** @brief Checks whether an expression exposes a locator-backed target sequence. */
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

    /** @brief Checks a restriction list for forbidden target-sequence evaluation. */
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

    /** @brief Validates a canonical sequence typmod supported by runtime dispatch. */
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

    /** @brief Raises a PostgreSQL datatype error for operands with different alphabets. */
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

    /** @brief Checks that two supported sequence typmods bind the same alphabet. */
    [[nodiscard]] bool mmsearch_typmods_share_alphabet(const int32 target_typmod,
                                                       const int32 query_typmod) noexcept {
        return batch_join_typmod_is_supported(target_typmod) &&
               batch_join_typmod_is_supported(query_typmod) &&
               compact_sequence_stdc_type_params::decode(target_typmod).alphabet_code ==
                   compact_sequence_stdc_type_params::decode(query_typmod).alphabet_code;
    }

    /** @brief Carries a validated sequence typmod and whether its outer descriptor was erased. */
    struct per_row_sequence_typmod_validation {
        int32 typmod = -1;
        bool descriptor_erased = false;
    };

    /** @brief Checks a function node against its exact catalog argument and result types. */
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

    /** @brief Checks an operator node against its exact catalog implementation signature. */
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

    /**
     * @brief Derives one exact typmod through supported sequence-preserving expressions.
     *
     * PostgreSQL does not propagate typmods through ordinary FuncExpr results,
     * even when the exact catalog function is sequence-type preserving.  Admit an
     * erased descriptor only after recursively validating one identical concrete
     * typmod at every sequence-valued leaf.  Non-sequence literal/array operands
     * are neutral only for the exact registered concat overloads.
     */
    [[nodiscard]] std::optional<per_row_sequence_typmod_validation>
    per_row_validate_sequence_typmod(Node* expression, bioseq_stub_oids const& oids) {
        check_stack_depth();
        if (expression == nullptr || exprType(expression) != oids.bioseq_seq_type_oid) {
            return std::nullopt;
        }
        const int32 descriptor_typmod = exprTypmod(expression);
        // Reconciles a validated leaf typmod with the enclosing expression descriptor.
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

    /**
     * @brief Recursively detects parallel execution anywhere below a planner path.
     *
     * Reject a Gather hidden below the common unary planner wrappers. Looking
     * only at cheapest_total_path->pathtype misses Projection/Material wrappers
     * and lets a parallel child enter an executor that owns one backend-local
     * query materialization.
     */
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

    /** @brief Couples an eligible base scan with its costing and locator targets. */
    struct batch_join_bioseq_path_candidate {
        CustomPath* scan = nullptr;
        Path* cost_owner = nullptr;
        const PathTarget* locator_target = nullptr;
    };

    /** @brief Reports whether an expression contains any match-result helper. */
    [[nodiscard]] bool batch_join_expression_has_match_helper(Node* expression,
                                                              const bioseq_stub_oids& oids);

    /** @brief Checks whether a path target contains a structurally equal expression. */
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

    /** @brief Decodes a match helper applied directly to the selected target relation. */
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

    /**
     * @brief Decodes a match helper with an exact catalog, arity, and result contract.
     *
     * Its
     * argument binding is resolved separately against the selected owner recipe
     * after standard_planner has assigned final plan_node_id values.
     */
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

    /** @brief Returns the sole argument of a well-formed match helper. */
    [[nodiscard]] Node* batch_join_match_helper_argument(FuncExpr* helper) noexcept {
        return helper == nullptr || list_length(helper->args) != 1
                   ? nullptr
                   : reinterpret_cast<Node*>(linitial(helper->args));
    }

    /** @brief Builds the canonical base-Var lineage for a match target recipe. */
    [[nodiscard]] List* canonical_match_base_lineage(Node* target_recipe,
                                                     const bioseq_stub_oids& oids);

    /** @brief Serializes a target recipe and its base lineage as an owner descriptor. */
    [[nodiscard]] List* make_match_owner_descriptor(Node* target_recipe,
                                                    Index base_relid,
                                                    const bioseq_stub_oids& oids);

    /** @brief Checks whether a helper names the exact transformed target recipe. */
    [[nodiscard]] bool batch_join_match_helper_names_recipe(FuncExpr* helper,
                                                            Node* target_recipe) noexcept {
        Node* argument = batch_join_match_helper_argument(helper);
        return argument != nullptr && target_recipe != nullptr && equal(argument, target_recipe);
    }

    /** @brief Checks whether a helper names one canonical base source of a recipe. */
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

    /**
     * @brief Decodes a match-helper PHV at a valid transformed-recipe frontier.
     *
     * Unlike the
     * base-lineage helper decoder below, the PHV may cover more than one relation when
     * the target recipe is a concat.  The final owner resolver validates the
     * canonical argument and lineage before any executor sees the site.
     */
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

    /**
     * @brief Decodes a direct target helper wrapped in its exact single-relation PHV.
     *
     * Do not accept a generic PHV merely because a helper occurs somewhere below
     * it because its evaluation and nulling frontier may belong to unrelated SQL.
     */
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

    /** @brief Checks for the exact single-relation helper PHV shape. */
    [[nodiscard]] bool batch_join_is_exact_helper_phv(const PlaceHolderVar* placeholder,
                                                      const Index target_relid) noexcept {
        return batch_join_exact_helper_phv_function(placeholder, target_relid) != nullptr;
    }

    /** @brief Tracks whether a projection delta contains only parent-safe helper support. */
    struct batch_join_projection_delta {
        const PathTarget* child_target = nullptr;
        Index target_relid = 0;
        bool saw_exact_helper_phv = false;
        bool invalid = false;
    };

    /** @brief Validates Vars and helper PHVs introduced by a projection wrapper. */
    struct batch_join_projection_delta_visitor {
        /** @brief Checks one wrapper-only expression node against the child target. */
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

    /**
     * @brief Checks that a wrapper-only expression can move to the BatchJoin parent.
     *
     * Validate that the complete wrapper-only expression can be evaluated by the
     * BatchJoin parent without changing semantics: it is nonvolatile, every Var
     * is already produced by the child, every PHV is the exact helper-support
     * shape, and at least one such helper PHV explains the wrapper delta.
     */
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

    /**
     * @brief Checks whether an expression is a safe non-sequence target Var.
     *
     * A wrapper may also expose an ordinary target attribute needed above the
     * join (for example t.pk).  Carrying that base Var through the locator child
     * is exact; carrying the sequence datum, a whole row, a system attribute, a
     * nulled outer-join Var, or an expression assembled at this level is not.
     */
    [[nodiscard]] bool batch_join_is_safe_plain_target_var(
        Node* expression, const Index target_relid, const AttrNumber target_seq_attno) noexcept {
        Var* var = try_cast_pg_node<Var>(expression);
        return var != nullptr && var->varno == target_relid && var->varattno > 0 &&
               var->varattno != target_seq_attno && var->varlevelsup == 0 &&
               bms_is_empty(var->varnullingrels);
    }

    /**
     * @brief Checks whether a base ProjectionPath can be evaluated by the parent owner.
     *
     * A helper support function can add a ProjectionPath at the base relation.
     * BioseqBatchJoin may look through it only when every wrapper-only expression
     * is either an exact helper expression evaluated by the parent or one plain,
     * non-sequence target Var copied into the locator child. Any other expression
     * is a real evaluation boundary and disqualifies this candidate.
     */
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

    /** @brief Finds the cheapest unparameterized Bioseq scan below parent-safe projection wrappers. */
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

    /**
     * @brief Holds a synthetic base scan and projection wrapper for eligibility tests.
     *
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

    /** @brief Builds an exact single-relation helper PHV for projection-guard tests. */
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

    /** @brief Builds a synthetic projection-wrapped Bioseq path for guard tests. */
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

    /** @brief Checks whether a path target contains an expression equal to a candidate. */
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

    /** @brief Collects target Vars needed below a parent-owned projection. */
    struct batch_join_underlying_target_var_collector {
        PathTarget* destination = nullptr;
        Index target_relid = 0;
        AttrNumber target_seq_attno = InvalidAttrNumber;
        bool invalid = false;
    };

    /** @brief Validates and copies safe target Vars into a locator child target. */
    struct batch_join_underlying_target_var_visitor {
        /** @brief Processes one Var dependency beneath a wrapper expression. */
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

    /** @brief Appends all safe underlying target Vars required by a wrapper target. */
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

} // export custom-scan partition declarations
