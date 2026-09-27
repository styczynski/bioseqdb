/**
 * @file
 * @brief Checks edit predicates and determines the alignment work requested by scan expressions.
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

export module bioseq.postgres.table:custom_scan_oid_expression_support_semantic_analysis;

import std;
import bioseq.postgres.configuration.aligner;
import bioseq.pipeline.minimap_port;
import bioseq.postgres.support;
import bioseq.sequences.compact_sequence_typmod;

import :custom_scan_scan_feature_contracts;

import :custom_scan_oid_expression_support_oid_catalog;
import :custom_scan_oid_expression_support_expression_analysis;
import :custom_scan_oid_expression_support_restriction_ownership;
export import :custom_scan_planner_aligner_contracts;
export import :custom_scan_oid_expression_support_semantic_analysis_contracts;

using namespace bioseq::postgres::support;

export {
    /**
     * @brief Checks that a sequence-valued edit argument names the aligned base column.
     *
     * A non-sequence row identifier passes this check and is checked by helper
     * registration. Calls to other functions also pass.
     */
    [[nodiscard]] bool
    edit_sequence_argument_matches_target(const FuncExpr* function,
                                          Index target_relid,
                                          AttrNumber target_sequence_attno,
                                          const bioseq_stub_oids& oids) noexcept {
        if (function == nullptr || function->funcid != oids.edit_oid)
            return true;
        if (list_length(function->args) != 1)
            return false;
        Node* argument_node = reinterpret_cast<Node*>(linitial(function->args));
        if (argument_node == nullptr || exprType(argument_node) != oids.bioseq_seq_type_oid) {
            /* Helper registration checks ownership of non-sequence row identifiers. */
            return true;
        }
        Var* argument = try_cast_pg_node<Var>(argument_node);
        if (argument == nullptr) {
            /* This check supports a direct target column. A sequence expression
             * needs comparison with the complete target in the recipe-aware check. */
            return false;
        }
        return target_relid != 0 && target_sequence_attno > 0 && argument->varno == target_relid &&
               argument->varattno == target_sequence_attno && argument->varlevelsup == 0 &&
               bms_is_empty(argument->varnullingrels);
    }

    /** @brief Checks that a sequence-valued edit argument equals the complete match target. */
    [[nodiscard]] bool
    edit_sequence_argument_matches_recipe(const FuncExpr* function,
                                          Node* target_expression,
                                          Index target_relid,
                                          AttrNumber target_sequence_attno,
                                          const bioseq_stub_oids& oids) noexcept {
        if (function == nullptr || function->funcid != oids.edit_oid)
            return true;
        if (list_length(function->args) != 1 || target_expression == nullptr) {
            return false;
        }
        Node* argument = reinterpret_cast<Node*>(linitial(function->args));
        /* A helper for a sliced or reversed target must name that complete
         * target expression. Naming only the stored sequence would attribute
         * the transformed match to a different sequence. */
        if (argument != nullptr && exprType(argument) == oids.bioseq_seq_type_oid) {
            return equal(argument, target_expression);
        }
        /* Non-sequence row identifiers reach this path. Helper registration
         * checks that they belong to the target relation. */
        return edit_sequence_argument_matches_target(
            function, target_relid, target_sequence_attno, oids);
    }

    /** @brief Finds the alignment features requested by output expressions and filters. */
    [[nodiscard]] scan_feature_flags
    detect_feature_flags(List* targetlist, List* qual, const bioseq_stub_oids& oids) {
        bioseq_feature_detector detector{&oids, scan_feature_flags{}};
        feature_flag_visitor visitor;
        for (auto tgt : postgres_list<TargetEntry>(targetlist)) {
            if (tgt == nullptr) {
                continue;
            }
            walk_pg_expression_including_root(to_pg_node(tgt->expr), detector, visitor);
        }
        for (auto q : postgres_list<Node>(qual)) {
            if (q == nullptr) {
                continue;
            }
            walk_pg_expression_including_root(q, detector, visitor);
        }
        return detector.flags;
    }

    /** @brief Tracks whether an expression contains a bioseq.edit call. */
    struct edit_function_probe {
        const bioseq_stub_oids* oids = nullptr;
        bool found = false;
    };

    /** @brief Stops an expression walk at the first bioseq.edit call. */
    struct edit_function_probe_visitor {
        /** @brief Marks an edit call as found and stops, or keeps searching the expression. */
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor, pg_node_view node, edit_function_probe& probe) const {
            if (FuncExpr* function = node.try_cast<FuncExpr>();
                function != nullptr && function->funcid == probe.oids->edit_oid) {
                probe.found = true;
                return pg_walk_step::stop;
            }
            return pg_walk_step::descend;
        }
    };

    /** @brief Tests whether an expression contains a bioseq.edit call. */
    [[nodiscard]] bool contains_bioseq_edit(Node* expression, const bioseq_stub_oids& oids) {
        if (expression == nullptr)
            return false;
        edit_function_probe probe{&oids, false};
        edit_function_probe_visitor visitor;
        walk_pg_expression_including_root(expression, probe, visitor);
        return probe.found;
    }

    /**
     * @brief Removes a PlaceHolderVar wrapper that belongs only to this scan.
     *
     * Returns the operand unchanged if it has no wrapper. Returns null for a
     * wrapper with another owner, an outer-query reference, or outer-join
     * nulling information.
     */
    [[nodiscard]] Node* strict_edit_operand(Node* operand, Index scan_relid) noexcept {
        PlaceHolderVar* placeholder = try_cast_pg_node<PlaceHolderVar>(operand);
        if (placeholder == nullptr)
            return operand;
        /* Keep the ownership and outer-join checks before unwrapping. Dropping
         * a generic PlaceHolderVar could change when its expression is evaluated
         * or whether an outer join makes it NULL. */
        if (!bioseq_is_concrete_rte_varno(scan_relid) || placeholder->phid == 0 ||
            placeholder->phlevelsup != 0 || !bms_is_empty(placeholder->phnullingrels) ||
            bms_num_members(placeholder->phrels) != 1 ||
            !bms_is_member(static_cast<int>(scan_relid), placeholder->phrels)) {
            return nullptr;
        }
        return reinterpret_cast<Node*>(placeholder->phexpr);
    }

    /**
     * @brief Extracts bioseq.edit(sequence_column) < integer_constant for scan execution.
     *
     * Require the column to belong to this scan and the comparison to use a
     * PostgreSQL built-in integer less-than function. Return no value for any
     * other form.
     */
    [[nodiscard]] std::optional<strict_edit_predicate> decode_strict_edit_upper_bound(
        Node* expression, Index scan_relid, const bioseq_stub_oids& oids) {
        OpExpr* comparison = try_cast_pg_node<OpExpr>(expression);
        if (comparison == nullptr || comparison->opresulttype != BOOLOID ||
            list_length(comparison->args) != 2) {
            return std::nullopt;
        }
        postgres_list<Node> arguments(comparison->args);
        FuncExpr* edit =
            try_cast_pg_node<FuncExpr>(strict_edit_operand(arguments.get_first(), scan_relid));
        Const* upper = try_cast_pg_node<Const>(arguments.get_second());
        if (edit == nullptr || edit->funcid != oids.edit_oid || edit->funcresulttype != INT8OID ||
            list_length(edit->args) != 1 || upper == nullptr || upper->constisnull) {
            return std::nullopt;
        }
        /* A literal such as 10 has type int4, so BIGINT < 10 uses int84lt.
         * Accept the built-in comparisons for all three integer widths and
         * read each constant using its actual type. Checking the function OID
         * also excludes user-defined operators named <. */
        const Oid opcode = get_opcode(comparison->opno);
        std::int64_t exclusive_upper_bound = 0;
        switch (upper->consttype) {
        case INT8OID:
            if (opcode != F_INT8LT)
                return std::nullopt;
            exclusive_upper_bound = DatumGetInt64(upper->constvalue);
            break;
        case INT4OID:
            if (opcode != F_INT84LT)
                return std::nullopt;
            exclusive_upper_bound = static_cast<std::int64_t>(DatumGetInt32(upper->constvalue));
            break;
        case INT2OID:
            if (opcode != F_INT82LT)
                return std::nullopt;
            exclusive_upper_bound = static_cast<std::int64_t>(DatumGetInt16(upper->constvalue));
            break;
        default:
            return std::nullopt;
        }
        Var* sequence = try_cast_pg_node<Var>(reinterpret_cast<Node*>(linitial(edit->args)));
        if (sequence == nullptr || sequence->varno != scan_relid || sequence->varattno <= 0 ||
            sequence->varlevelsup != 0 || !bms_is_empty(sequence->varnullingrels) ||
            sequence->vartype != oids.bioseq_seq_type_oid) {
            return std::nullopt;
        }
        return strict_edit_predicate{
            .exclusive_upper_bound = exclusive_upper_bound,
            .sequence_attno = sequence->varattno,
        };
    }

    /** @brief Builds a filter list excluding whole expressions that contain bioseq.edit. */
    [[nodiscard]] List* quals_without_bioseq_edit(List* quals, const bioseq_stub_oids& oids) {
        postgres_list<Node> filtered;
        for (Node* qual : postgres_list<Node>(quals)) {
            if (qual != nullptr && !contains_bioseq_edit(qual, oids)) {
                filtered.append(qual);
            }
        }
        return filtered.release();
    }

    /** @brief Finds requested alignment features, returning no value for malformed restrictions. */
    [[nodiscard]] std::optional<scan_feature_flags>
    detect_restrictinfo_feature_flags(List* restrictions, const bioseq_stub_oids& oids) {
        bioseq_feature_detector detector{&oids, scan_feature_flags{}};
        feature_flag_visitor visitor;
        for (Node* node : postgres_list<Node>(restrictions)) {
            RestrictInfo* restriction = try_cast_pg_node<RestrictInfo>(node);
            if (restriction == nullptr || restriction->clause == nullptr) {
                return std::nullopt;
            }
            walk_pg_expression_including_root(
                reinterpret_cast<Node*>(restriction->clause), detector, visitor);
        }
        return detector.flags;
    }

    /** @brief Keeps the first storage-width conversion found for EXPLAIN. */
    struct explain_coalesce_walk_context {
        const bioseq_stub_oids* oids;
        explain_typmod_coalesce result;
    };

    /** @brief Finds mmsearch operands with the same alphabet and different storage-unit widths. */
    struct typmod_coalesce_visitor {
        /** @brief Records the first supported operand-width mismatch and keeps walking. */
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              explain_coalesce_walk_context& ctx) const {
            if (!ctx.result.active) {
                if (FuncExpr* fe = node.try_cast<FuncExpr>()) {
                    if (is_mmsearch_funcid(fe->funcid, *ctx.oids) && list_length(fe->args) >= 2) {
                        Node* arg0 =
                            reinterpret_cast<Node*>(linitial(fe->args)); // target sequence column
                        Node* arg1 = reinterpret_cast<Node*>(
                            lsecond(fe->args)); // query constant, parameter, or column
                        if (arg0 != nullptr && arg1 != nullptr &&
                            pg_node_view{arg0}.is_type<Var>()) {
                            const int32 col_typmod = expect_pg_node<Var>(arg0)->vartypmod;
                            int32 arg1_typmod = -1;
                            if (Const* arg1_const = try_cast_pg_node<Const>(arg1)) {
                                arg1_typmod = arg1_const->consttypmod;
                            } else if (Param* arg1_param = try_cast_pg_node<Param>(arg1)) {
                                arg1_typmod = arg1_param->paramtypmod;
                            } else if (Var* arg1_var = try_cast_pg_node<Var>(arg1)) {
                                arg1_typmod = arg1_var->vartypmod;
                            }
                            if (col_typmod >= 0 && arg1_typmod >= 0) {
                                const auto col =
                                    compact_sequence_stdc_type_params::decode(col_typmod);
                                const auto qry =
                                    compact_sequence_stdc_type_params::decode(arg1_typmod);
                                if (col.alphabet_code == qry.alphabet_code &&
                                    col.storage_unit_byte_width != qry.storage_unit_byte_width) {
                                    ctx.result.active = true;
                                    ctx.result.col_alpha = col.alphabet_code;
                                    ctx.result.col_byte_width = col.storage_unit_byte_width;
                                }
                            }
                        }
                    }
                }
            }
            return pg_walk_step::descend;
        }
    };

    /**
     * @brief Finds a sequence-width conversion that EXPLAIN can report before execution.
     *
     * Inspect mmsearch calls with known operand types. If their alphabets agree
     * and their storage-unit widths differ, report the target column's width.
     */
    [[nodiscard]] explain_typmod_coalesce
    explain_detect_typmod_coalesce(List* qual, const bioseq_stub_oids& oids) {
        explain_coalesce_walk_context ctx{&oids, explain_typmod_coalesce{}};
        typmod_coalesce_visitor visitor;
        for (auto q : postgres_list<Node>(qual)) {
            if (q != nullptr) {
                walk_pg_expression_including_root(q, ctx, visitor);
            }
        }
        return ctx.result;
    }

    /** @brief Copies PostgreSQL's current aligner settings into a pipeline parameter set. */
    [[nodiscard]] bioseq::simd::minimap2_parameters build_params_from_gucs() {
        bioseq::simd::minimap2_parameters params;
        params.kmer_size = bioseq::aligner::settings::minimizer::kmer_size;
        params.window_size = bioseq::aligner::settings::minimizer::window_size;
        params.max_gap = bioseq::aligner::settings::chaining::maximum_gap;
        params.max_gap_ref = bioseq::aligner::settings::chaining::maximum_reference_gap;
        params.bandwidth = bioseq::aligner::settings::chaining::bandwidth;
        params.max_chain_skip = bioseq::aligner::settings::chaining::maximum_skipped_anchors;
        params.max_chain_iter = bioseq::aligner::settings::chaining::maximum_predecessors;
        params.min_seed_count = bioseq::aligner::settings::chaining::minimum_seed_count;
        params.min_chain_score = bioseq::aligner::settings::chaining::minimum_score;
        params.chain_gap_scale =
            static_cast<float>(bioseq::aligner::settings::chaining::gap_penalty_scale);
        params.chain_skip_scale =
            static_cast<float>(bioseq::aligner::settings::chaining::skip_penalty_scale);
        params.mask_level =
            static_cast<float>(bioseq::aligner::settings::secondary::overlap_threshold);
        params.mask_len = bioseq::aligner::settings::secondary::uncovered_length_limit;
        params.pri_ratio =
            static_cast<float>(bioseq::aligner::settings::secondary::score_ratio_threshold);
        params.best_n = bioseq::aligner::settings::secondary::maximum_count;
        params.match_score = bioseq::aligner::settings::scoring::match_score;
        params.mismatch_pen = bioseq::aligner::settings::scoring::mismatch_penalty;
        params.gap_open1 = bioseq::aligner::settings::scoring::short_gap_open_penalty;
        params.gap_ext1 = bioseq::aligner::settings::scoring::short_gap_extension_penalty;
        params.gap_open2 = bioseq::aligner::settings::scoring::long_gap_open_penalty;
        params.gap_ext2 = bioseq::aligner::settings::scoring::long_gap_extension_penalty;
        params.sc_ambi = bioseq::aligner::settings::scoring::ambiguous_base_penalty;
        params.zdrop = bioseq::aligner::settings::scoring::z_drop_threshold;
        params.end_bonus = bioseq::aligner::settings::scoring::end_bonus;
        params.max_occ = bioseq::aligner::settings::seeding::maximum_occurrence;
        params.mid_occ = bioseq::aligner::settings::seeding::intermediate_occurrence_threshold;
        params.block_min_size =
            static_cast<size_t>(bioseq::aligner::settings::block_alignment::minimum_block_size);
        params.block_max_size =
            static_cast<size_t>(bioseq::aligner::settings::block_alignment::maximum_block_size);
        params.bandwidth_long = bioseq::aligner::settings::chaining::long_join_bandwidth;
        params.chaining_algorithm = static_cast<bioseq::simd::chaining_algorithm>(
            static_cast<int>(bioseq::aligner::settings::chaining::algorithm.get_value()));
        params.base_alignment_algorithm = static_cast<bioseq::simd::base_alignment_algorithm>(
            static_cast<int>(bioseq::aligner::settings::base_alignment::algorithm.get_value()));
        params.minimizer_algorithm = static_cast<bioseq::simd::minimizer_algorithm>(
            static_cast<int>(bioseq::aligner::settings::minimizer::algorithm.get_value()));
        params.enable_profiling = bioseq::aligner::settings::profiling::enabled;
        params.vulkan_enabled = bioseq::aligner::settings::vulkan::enabled;
        params.vulkan_seed_hits_enabled = bioseq::aligner::settings::vulkan::seed_lookup::enabled;
        params.vulkan_index_cache_enabled = bioseq::aligner::settings::vulkan::index_cache::enabled;
        /* Settings for the fused GPU frontend and GPU index construction. */
        params.vulkan_fused_frontend_enabled =
            bioseq::aligner::settings::vulkan::fused_frontend::enabled;
        params.vulkan_gpu_index_build_enabled =
            bioseq::aligner::settings::vulkan::index_build::enabled;
        params.vulkan_extend_enabled =
            bioseq::aligner::settings::vulkan::alignment_extension::enabled;
        params.vulkan_ordered_sketch_tile_bp =
            static_cast<uint32_t>(bioseq::aligner::settings::vulkan::ordered_sketch_tile_bp);
        params.vulkan_radix_digit_bits =
            static_cast<uint32_t>(bioseq::aligner::settings::vulkan::radix_sort_digit_bits);
        return params;
    }

} // export custom-scan partition declarations
