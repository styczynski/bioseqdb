/**
 * @file
 * @brief Connects base-scan startup and, expression placeholders to PostgreSQL execution.
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

export module bioseq.postgres.table:custom_scan_join_runtime_tuple_production;

import std;
import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.types.compact_sequence;
import bioseq.pipeline.minimap_port;
import bioseq.sequences.cigar;
import bioseq.sequences.compact_sequence_typmod;
import bioseq.postgres.storage.indexing.admin.policy_catalog;

import :custom_scan_scan_runtime_state_contracts;
import :custom_scan_runtime_placeholder_contracts;
import :custom_scan_runtime_match_contracts;
import :custom_scan_oid_expression_support_oid_catalog_contracts;
import :custom_scan_oid_expression_support_semantic_analysis_contracts;
import :custom_scan_join_ownership_helper_contracts;
import :custom_scan_join_runtime_tuple_injection_contracts;
import :custom_scan_join_runtime_scan_fetch_contracts;
import :custom_scan_join_runtime_scan_admission_contracts;

using namespace bioseq::postgres::support;

export {
    /**
     * @brief Starts a base scan and reports failures through PostgreSQL.
     *
     * On failure, abandon the dispatch buffer before raising the error. Preserve
     * PostgreSQL errors and translate  C++ exceptions into internal errors.
     */
    void
    bioseq_column_scan_begin_custom_scan(CustomScanState* cscanstate, EState* estate, int eflags) {
        ErrorData* exact_error = nullptr;
        bool cpp_error = false;
        bool unknown_cpp_error = false;
        std::array<char, 512> detail{"std::exception without detail"};
        try {
            bioseq_column_scan_begin_custom_scan_impl(cscanstate, estate, eflags);
            return;
        } catch (bioseq::postgres::support::postgres_error& error) {
            exact_error = error.release_error_data();
        } catch (const std::exception& error) {
            strlcpy(detail.data(),
                    error.what() == nullptr ? "std::exception without detail" : error.what(),
                    detail.size());
            cpp_error = true;
        } catch (...) {
            unknown_cpp_error = true;
        }

        scan_state* state = scan_state::from_custom_scan_state(cscanstate);
        if (state != nullptr && state->runtime_state != nullptr) {
            auto* runtime = static_cast<bioseq_scan_runtime*>(state->runtime_state);
            runtime->buffer.abandon_join_runtime_after_pg_error_noexcept();
        }
        if (exact_error != nullptr) {
            ReThrowError(exact_error);
        }
        if (cpp_error) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqColumnScan Begin raised a C++ exception"),
                     errdetail_internal("%s", detail.data())));
        }
        if (unknown_cpp_error) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqColumnScan Begin raised an unknown C++ exception")));
        }
    }

    /**
     * @brief Finds the writable Const for a match helper.
     *
     * bioseq.score and bioseq.dp_score share the score placeholder. Return null for
     * a function that has no match placeholder.
     */
    Const* lookup_bioseq_placeholder_const(Oid funcid, const var_injector_context* ctx) {
        const bioseq_stub_oids* o = ctx->oids;
        const placeholder_constants* c = ctx->consts;
        if (funcid == o->score_oid || funcid == o->dp_score_oid)
            return c->dp_score_node;
        if (funcid == o->query_len_oid)
            return c->query_len_node;
        if (funcid == o->query_start_oid)
            return c->query_start_node;
        if (funcid == o->query_end_oid)
            return c->query_end_node;
        if (funcid == o->target_len_oid)
            return c->target_len_node;
        if (funcid == o->target_start_oid)
            return c->target_start_node;
        if (funcid == o->target_end_oid)
            return c->target_end_node;
        if (funcid == o->num_matches_oid)
            return c->num_matches_node;
        if (funcid == o->block_length_oid)
            return c->block_length_node;
        if (funcid == o->edit_oid)
            return c->edit_distance_node;
        if (funcid == o->mapq_oid)
            return c->mapq_node;
        if (funcid == o->chain_score_oid)
            return c->chain_score_node;
        if (funcid == o->dp_max_score_oid)
            return c->dp_max_score_node;
        if (funcid == o->seed_count_oid)
            return c->seed_count_node;
        if (funcid == o->secondary_chain_score_oid)
            return c->secondary_chain_score_node;
        if (funcid == o->is_reverse_oid)
            return c->is_reverse_node;
        if (funcid == o->is_primary_oid)
            return c->is_primary_node;
        if (funcid == o->has_cigar_oid)
            return c->has_cigar_node;
        if (funcid == o->divergence_oid)
            return c->divergence_node;
        if (funcid == o->cigar_oid)
            return c->cigar_node;
        return nullptr;
    }

    /** @brief Checks that the planner bound this helper call to the current scan and match ID. */
    [[nodiscard]] bool
    base_match_helper_is_finalized_for_scan(FuncExpr* function,
                                            const var_injector_context& context) {
        if (function == nullptr || context.oids == nullptr || context.stable_match_id < 0 ||
            context.match_helper_sites == NIL ||
            batch_join_well_formed_match_helper(reinterpret_cast<Node*>(function), *context.oids) ==
                nullptr) {
            return false;
        }
        Node* argument = batch_join_match_helper_argument(function);
        if (argument == nullptr)
            return false;
        for (List* site : postgres_list<List>(context.match_helper_sites)) {
            if (site == nullptr || !IsA(site, List) ||
                list_length(site) != k_bioseq_match_helper_site_fields)
                continue;
            Node* canonical_argument =
                reinterpret_cast<Node*>(list_nth(site, k_match_site_canonical_argument));
            if (static_cast<Oid>(intVal(list_nth(site, k_match_site_funcid))) == function->funcid &&
                (static_cast<match_helper_binding_kind>(
                     intVal(list_nth(site, k_match_site_binding_kind))) ==
                     match_helper_binding_kind::exact_recipe ||
                 static_cast<match_helper_binding_kind>(
                     intVal(list_nth(site, k_match_site_binding_kind))) ==
                     match_helper_binding_kind::unique_lineage) &&
                intVal(list_nth(site, k_match_site_stable_id)) == context.stable_match_id &&
                canonical_argument != nullptr && equal(canonical_argument, argument))
                return true;
        }
        return false;
    }

    /**
     * @brief Replaces a supported symbol_count call with a counm supplied for each row.
     *
     * The sequence must be a column with a known alphabet. The letter must be a
     * non-null, one-byte text constant in that alphabet. Record the column and
     * letter so the  executor can obtain its count for each row.
     *
     * @return The new BIGINT placeholder, or null to keep the scalar function call.
     */
    Const* try_rewrite_symbol_count(FuncExpr* fe, var_injector_context* ctx) {
        if (ctx == nullptr || fe == nullptr || ctx->consts == nullptr) {
            return nullptr;
        }
        if (list_length(fe->args) != 2) {
            return nullptr;
        }
        postgres_list<Node> fe_args(fe->args);
        Node* arg0 = fe_args.get_first();
        Node* arg1 = fe_args.get_second();
        if (arg0 == nullptr || arg1 == nullptr) {
            return nullptr;
        }
        Var* col_var = try_cast_pg_node<Var>(arg0);
        Const* letter_const = try_cast_pg_node<Const>(arg1);
        if (col_var == nullptr || letter_const == nullptr) {
            bioseq_log<logging_level::debug>("[bioseq] pg: var_injector_mutator() leaving "
                                             "symbol_count(...) alone; arg0_tag={} arg1_tag={}",
                                             static_cast<int>(nodeTag(arg0)),
                                             static_cast<int>(nodeTag(arg1)));
            return nullptr;
        }
        if (col_var->vartypmod < 0 || col_var->vartype != ctx->oids->bioseq_seq_type_oid) {
            bioseq_log<logging_level::debug>("[bioseq] pg: VarInjectorMutator() symbol_count arg0 "
                                             "has wrong type/typmod (type={}, typmod={})",
                                             col_var->vartype,
                                             col_var->vartypmod);
            return nullptr;
        }
        if (letter_const->constisnull) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: VarInjectorMutator() symbol_count arg1 is NULL; skipping");
            return nullptr;
        }

        text* letter_text = reinterpret_cast<text*>(DatumGetPointer(letter_const->constvalue));
        if (letter_text == nullptr) {
            return nullptr;
        }
        text* letter_detoasted =
            reinterpret_cast<text*>(PG_DETOAST_DATUM(letter_const->constvalue));
        const int32 letter_size = VARSIZE_ANY_EXHDR(letter_detoasted);
        if (letter_size != 1) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: VarInjectorMutator() symbol_count arg1 size={} (expected 1)",
                letter_size);
            if (reinterpret_cast<void*>(letter_detoasted) !=
                reinterpret_cast<void*>(DatumGetPointer(letter_const->constvalue))) {
                pfree(letter_detoasted);
            }
            return nullptr;
        }
        const char letter_char = *VARDATA_ANY(letter_detoasted);
        if (reinterpret_cast<void*>(letter_detoasted) !=
            reinterpret_cast<void*>(DatumGetPointer(letter_const->constvalue))) {
            pfree(letter_detoasted);
        }

        const compact_sequence_stdc_type_params typmod =
            compact_sequence_stdc_type_params::decode(col_var->vartypmod);
        const int rank =
            bioseq::storage::indexing::letter_to_rank(typmod.alphabet_code, letter_char);
        if (rank < 0) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: VarInjectorMutator() symbol_count letter '{}' not in "
                "alphabet_code={}; keeping scalar call",
                letter_char,
                static_cast<unsigned>(typmod.alphabet_code));
            return nullptr;
        }

        /* Store the symbol count as a by-value BIGINT. Leave it NULL until
         * the executor has found the count for this row */
        Const* placeholder =
            makeConst(INT8OID,
                      -1,
                      InvalidOid,
                      8,
                      bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(0)).raw(),
                      /*constisnull=*/true,
                      /*constbyval=*/true);

        symbol_count_call_site site;
        site.placeholder = placeholder;
        site.rank = rank;
        site.letter = letter_char;
        site.column_attno = static_cast<int16_t>(col_var->varattno);
        if (ctx->current_relation != nullptr && col_var->varattno >= 1) {
            char* attname_cstr = get_attname(RelationGetRelid(ctx->current_relation),
                                             col_var->varattno,
                                             /*missing_ok=*/true);
            if (attname_cstr != nullptr) {
                site.column_name = std::string(attname_cstr);
                pfree(attname_cstr);
            }
        }
        bioseq_log<logging_level::debug>("[bioseq] pg: VarInjectorMutator() installing "
                                         "symbol_count placeholder column='{}' letter='{}' rank={}",
                                         site.column_name,
                                         letter_char,
                                         rank);

        ctx->consts->symbol_count_sites.push_back(std::move(site));
        return placeholder;
    }

    /** @brief Replaces bound match helpers and supported symbol counts with writable Const nodes. */
    struct var_injector_mutator {
        /** @brief Rewrites one expression node and leaves PlaceHolderVar wrappers intact. */
        [[nodiscard]] pg_node_mutation operator()(pg_expression_mutation_cursor,
                                                  pg_node_view node,
                                                  var_injector_context& injector_context) const {
            if (FuncExpr* func_expr = node.try_cast<FuncExpr>()) {
                if (!edit_sequence_argument_matches_recipe(func_expr,
                                                           injector_context.match_target_recipe,
                                                           injector_context.match_sequence_varno,
                                                           injector_context.match_sequence_attno,
                                                           *injector_context.oids)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                             errmsg("bioseq.edit projection must reference the mmsearch target "
                                    "column")));
                }
                Const* replacement =
                    base_match_helper_is_finalized_for_scan(func_expr, injector_context)
                        ? lookup_bioseq_placeholder_const(func_expr->funcid, &injector_context)
                        : nullptr;
                if (replacement != nullptr) {
                    bioseq_log<logging_level::debug>(
                        "[bioseq] pg: var_injector_mutator() replacing bioseq funcid={} with "
                        "placeholder Const",
                        func_expr->funcid);
                    return pg_node_mutation::replace_with(to_pg_node(replacement));
                }
                if (func_expr->funcid == injector_context.oids->symbol_count_oid) {
                    Const* symbol_placeholder =
                        try_rewrite_symbol_count(func_expr, &injector_context);
                    if (symbol_placeholder != nullptr) {
                        return pg_node_mutation::replace_with(to_pg_node(symbol_placeholder));
                    }
                    /* Keep unsupported calls here so PostgreSQL evaluates symbol_count
                     * through its scalar function. */
                }
                /* Search  other function arguments for helper calls. */
            }
            /* PostgreSQL should resolve PlaceHolderVars before, execution. Keep any
             * remaining wrapper while visiting its expression: removing it here
             * would lose the outer-join information in phnullingrels. */
            if (PlaceHolderVar* phv = node.try_cast<PlaceHolderVar>()) {
                bioseq_log<logging_level::debug>(
                    "[bioseq] pg: var_injector_mutator() encountered PlaceHolderVar phid={} at "
                    "exec time — passing through",
                    phv->phid);
                /* fall through to descend */
            }
            return pg_node_mutation::descend();
        }
    };

    /** @brief Walks a scan expression and installs the match and symbol-count placeholders. */
    [[nodiscard]] Node* var_injector(Node* node, var_injector_context* injector_context) {
        var_injector_mutator mutator;
        return mutate_pg_expression(node, *injector_context, mutator);
    }

    /**
     * @brief Fills the installed match placeholders fhom one alignment result.
     *
     * Skip placeholders that are absent. Allocate the CIGAR sequence in the current
     * PostgreSQL memory context, where it remains available  for row projection.
     */
    void populate_placeholder_consts(placeholder_constants& consts,
                                     const bioseq::simd::match_result& match) {
        // Assert that the value fits in INT4 before filling its placeholder.
        auto set_int = [](Const* node, int64_t value) {
            if (node == nullptr)
                return;
            Assert(value >= std::numeric_limits<std::int32_t>::min() &&
                   value <= std::numeric_limits<std::int32_t>::max());
            node->constvalue =
                bioseq::postgres::support::pg_datum::from(static_cast<std::int32_t>(value)).raw();
            node->constisnull = false;
        };
        // Sequence positions, lengths, and edit distance use BIGINT so values above
        // INT32_MAX survive projection. Assert that each value fits in signed int64.
        auto set_bigint = [](Const* node, std::uint64_t value) {
            if (node == nullptr)
                return;
            Assert(value <=
                   static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
            node->constvalue =
                bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(value)).raw();
            node->constisnull = false;
        };
        // Stores a match flag in a boolean placeholder when one was installed.
        auto set_bool = [](Const* node, bool value) {
            if (node == nullptr)
                return;
            node->constvalue = bioseq::postgres::support::pg_datum::from(value).raw();
            node->constisnull = false;
        };
        // Stores a match metric in a FLOAT4 placeholder when one was installed.
        auto set_float = [](Const* node, float value) {
            if (node == nullptr)
                return;
            node->constvalue = bioseq::postgres::support::pg_datum::from(value).raw();
            node->constisnull = false;
        };

        set_int(consts.dp_score_node, match.dp_score);
        // Preserve 64-bit sequence coordinates and counts in SQL results.
        set_bigint(consts.query_len_node, match.query_len);
        set_bigint(consts.query_start_node, match.query_start);
        set_bigint(consts.query_end_node, match.query_end);
        set_bigint(consts.target_len_node, match.target_len);
        set_bigint(consts.target_start_node, match.target_start);
        set_bigint(consts.target_end_node, match.target_end);
        set_bigint(consts.num_matches_node, match.num_matches);
        set_bigint(consts.block_length_node, match.block_length);
        set_bigint(consts.edit_distance_node, match.edit_distance);
        set_int(consts.mapq_node, match.mapq);
        set_int(consts.chain_score_node, match.chain_score);
        set_int(consts.dp_max_score_node, match.dp_max_score);
        set_int(consts.seed_count_node, match.seed_count);
        set_int(consts.secondary_chain_score_node, match.secondary_chain_score);
        set_bool(consts.is_reverse_node, match.is_reverse);
        set_bool(consts.is_primary_node, match.is_primary);
        set_bool(consts.has_cigar_node, match.has_cigar);
        set_float(consts.divergence_node, match.divergence);

        if (consts.cigar_node != nullptr) {
            consts.cigar_node->constisnull = !match.has_cigar;
            consts.cigar_node->constvalue = match.has_cigar
                ? bioseq::postgres::support::pg_datum::from_pointer(make_pg_cigar_from_runs(
                      std::span<const bioseq::cigar_run>{match.cigar.data(), match.cigar.size()})).raw()
                : Datum{0};
        }
    }

} // export custom-scan partition declarations
