/**
 * @file
 * @brief Analyzes projection, edit predicates, feature flags, and default params.
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

import :custom_scan_runtime_support;

import :custom_scan_oid_expression_support_oid_catalog;
import :custom_scan_oid_expression_support_expression_analysis;
import :custom_scan_oid_expression_support_restriction_ownership;

using namespace bioseq::postgres::support;

export {
    /** @brief Checks that a sequence-valued edit helper names the base scan's aligned column. */
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
            /* Accept the target-owned row-marker helper form. */
            return true;
        }
        Var* argument = try_cast_pg_node<Var>(argument_node);
        if (argument == nullptr) {
            /* A sequence-valued wrapper (COALESCE, cast, PHV expression, etc.)
             * has biological identity that the shared match_result cannot safely
             * infer.  Only the exact mmsearch target Var is supported. */
            return false;
        }
        return target_relid != 0 && target_sequence_attno > 0 && argument->varno == target_relid &&
               argument->varattno == target_sequence_attno && argument->varlevelsup == 0 &&
               bms_is_empty(argument->varnullingrels);
    }

    /** @brief Checks that a sequence-valued edit helper names the complete owned target recipe. */
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
        /* A sequence-valued helper argument is biological identity, not a
         * row marker.  It must name the complete owned recipe; accepting the raw
         * storage Var of a sliced/reversed/concatenated target would silently report
         * the transformed match as if it belonged to the whole stored sequence. */
        if (argument != nullptr && exprType(argument) == oids.bioseq_seq_type_oid) {
            return equal(argument, target_expression);
        }
        /* Only non-sequence row-marker spellings reach the target-owned
         * fallback.  The surrounding helper registration has already validated
         * that such a Var belongs to target_relid. */
        return edit_sequence_argument_matches_target(
            function, target_relid, target_sequence_attno, oids);
    }

    /** @brief Accumulates executor feature requirements discovered in expression trees. */
    struct bioseq_feature_detector {
        const bioseq_stub_oids* oids = nullptr;
        scan_feature_flags flags;
    };

    /** @brief Maps mmsearch and projection calls to executor feature flags. */
    struct feature_flag_visitor {
        /** @brief Updates feature flags for the current node and continues traversal. */
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              bioseq_feature_detector& detector) const {
            if (FuncExpr* fe = node.try_cast<FuncExpr>()) {
                const bioseq_stub_oids* o = detector.oids;
                if (is_mmsearch_funcid(fe->funcid, *o)) {
                    detector.flags.has_mmsearch = true;
                }
                if (fe->funcid == o->cigar_oid || fe->funcid == o->has_cigar_oid) {
                    detector.flags.uses_cigar = true;
                }
                if (fe->funcid == o->edit_oid) {
                    detector.flags.uses_edit = true;
                }
                if (fe->funcid == o->divergence_oid) {
                    detector.flags.uses_divergence = true;
                }
                if (fe->funcid == o->mapq_oid) {
                    detector.flags.uses_mapq = true;
                }
                if (is_bioseq_projection_funcid(fe->funcid, *o)) {
                    detector.flags.any_match_projection = true;
                }
            }
            return pg_walk_step::descend;
        }
    };

    /** @brief Detects executor features referenced by target-list and qual expressions. */
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

    /** @brief Serialization constants for an extracted strict edit predicate. */
    inline constexpr int k_bioseq_edit_predicate_magic = 0x42534531; /* BSE1 */
    inline constexpr int k_bioseq_edit_predicate_version = 1;

    /** @brief Describes an executor-owned strict upper bound on edit distance. */
    struct strict_edit_predicate {
        std::int64_t exclusive_upper_bound = 0;
        AttrNumber sequence_attno = InvalidAttrNumber;
    };

    /** @brief Tracks whether an expression contains a bioseq.edit call. */
    struct edit_function_probe {
        const bioseq_stub_oids* oids = nullptr;
        bool found = false;
    };

    /** @brief Stops an expression walk at the first bioseq.edit call. */
    struct edit_function_probe_visitor {
        /** @brief Records an edit call at the current node and controls traversal. */
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

    /** @brief Unwraps the scan-owned placeholder form admitted for an edit predicate operand. */
    [[nodiscard]] Node* strict_edit_operand(Node* operand, Index scan_relid) noexcept {
        PlaceHolderVar* placeholder = try_cast_pg_node<PlaceHolderVar>(operand);
        if (placeholder == nullptr)
            return operand;
        /* bioseq_placeholder_support introduces this exact wrapper for aggregate
         * and join plans.  Accept only a scan-owned, non-nulled PHV; unwrapping a
         * generic or outer-join-nullable PHV would discard planner semantics. */
        if (!bioseq_is_concrete_rte_varno(scan_relid) || placeholder->phid == 0 ||
            placeholder->phlevelsup != 0 || !bms_is_empty(placeholder->phnullingrels) ||
            bms_num_members(placeholder->phrels) != 1 ||
            !bms_is_member(static_cast<int>(scan_relid), placeholder->phrels)) {
            return nullptr;
        }
        return reinterpret_cast<Node*>(placeholder->phexpr);
    }

    /** @brief Decodes an executor-owned `bioseq.edit(Var) < integer Const` predicate. */
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
        /* Operator names are schema-overloadable.  A bare SQL literal such as 10
         * is int4, so PostgreSQL resolves BIGINT < 10 to its built-in int84lt
         * implementation rather than inserting an int8 cast.  Accept only the
         * three exact built-in RHS integer widths and normalize their Const datum;
         * an arbitrary operator merely named `<` must remain fail-closed. */
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

    /** @brief Copies qualifiers while excluding expressions containing bioseq.edit. */
    [[nodiscard]] List* quals_without_bioseq_edit(List* quals, const bioseq_stub_oids& oids) {
        postgres_list<Node> filtered;
        for (Node* qual : postgres_list<Node>(quals)) {
            if (qual != nullptr && !contains_bioseq_edit(qual, oids)) {
                filtered.append(qual);
            }
        }
        return filtered.release();
    }

    /** @brief Detects feature flags in RestrictInfo clauses, rejecting unexpected list entries. */
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

    /** @brief Describes a plan-time sequence storage-width coalescing decision. */
    struct explain_typmod_coalesce {
        bool active = false;
        uint8_t col_alpha = 0;
        uint8_t col_byte_width = 0;
    };
    /** @brief Carries OIDs and the first detected typmod mismatch through an explain walk. */
    struct explain_coalesce_walk_context {
        const bioseq_stub_oids* oids;
        explain_typmod_coalesce result;
    };

    /** @brief Detects an mmsearch whose operands share an alphabet but not storage width. */
    struct typmod_coalesce_visitor {
        /** @brief Records the first qualifying typmod mismatch and continues traversal. */
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              explain_coalesce_walk_context& ctx) const {
            if (!ctx.result.active) {
                if (FuncExpr* fe = node.try_cast<FuncExpr>()) {
                    if (is_mmsearch_funcid(fe->funcid, *ctx.oids) && list_length(fe->args) >= 2) {
                        Node* arg0 =
                            reinterpret_cast<Node*>(linitial(fe->args)); // bioseq.seq column
                        Node* arg1 = reinterpret_cast<Node*>(
                            lsecond(fe->args)); // query literal / param / var
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

    /** @brief Derives typmod coalescing metadata from plan qualifiers without runtime state. */
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

    /** @brief Snapshots backend aligner settings into the parameters used to build a pipeline. */
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
        /* Resident two-submit frontend and GPU index construction. */
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
