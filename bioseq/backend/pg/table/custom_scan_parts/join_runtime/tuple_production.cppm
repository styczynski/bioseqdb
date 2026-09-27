/**
 * @file
 * @brief Owns scan fetch support and finalized placeholder population.
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
    /** @brief Initializes a base scan and translates all C++ failures to PostgreSQL errors. */
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
     * @brief Maps a supported match-helper function to its executor placeholder.
     *
     * bioseq.score aliases bioseq.dp_score, so both identifiers resolve to the
     * same placeholder.
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

    /** @brief Checks that a helper has a serialized binding for this base scan. */
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
     * @brief Rewrites an eligible symbol-count call to a per-row placeholder.
     *
     * Rewrite a single `bioseq.symbol_count(Var, 'L')` call site into a
     * placeholder Const that the executor populates per-row from meta_sidecar.
     * Returns the replacement Const*, or nullptr when the FuncExpr does not
     * match the supported shape (in which case the caller must leave the
     * original FuncExpr in place so the scalar O(n) path runs).
     *
     * Matching is deliberately strict: arg0 must be a Var (a column reference
     * with a concrete bioseq.seq typmod), and arg1 must be a single-byte
     * non-null text Const whose letter is in the column's alphabet. Any call
     * outside this shape — e.g. symbol_count('ACGT'::bioseq.seq, 'A') — keeps
     * its original FuncExpr and runs through the scalar entry point in
     * mod.cppm, which is cost-inflated by bioseq_symbol_count_support.
     *
     * @return The replacement Const, or null when the scalar call must remain.
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

        /*
         * Build the placeholder Const. INT8 is byval on LP64 (pg_type.typbyval
         * for bigint is 't'), so constbyval=true + constlen=8 is correct. The
         * initial value is NULL until the executor populates it per-row.
         */
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

    /** @brief Replaces finalized match helpers and eligible symbol counts with placeholders. */
    struct var_injector_mutator {
        /** @brief Mutates one expression node while preserving unresolved PHV semantics. */
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
                    /* Call shape not supported (literal arg, non-Const letter,
                     * letter outside alphabet, …) — let the scalar entry point
                     * in mod.cppm handle it in place. */
                }
                /* Fall through to recurse into arg subtrees of unrelated FuncExprs. */
            }
            /*
             * PlaceHolderVar handling. By Postgres convention the planner
             * resolves PHVs to Vars / Params by execution time (pathnodes.h:
             * "it is not recognized by the parser or executor"), so seeing
             * one here is a planner bug. We log loudly and pass the node
             * through — unwrapping would silently drop the PHV's
             * phnullingrels metadata and could produce wrong results in
             * outer-join nullable positions (a plain Const has no nulling
             * semantics). Returning descend() does the right thing because
             * the cursor recurses into phexpr, giving var_injector_mutator a
             * chance to rewrite our FuncExpr in place; the PHV node itself is
             * preserved.
             */
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

    /** @brief Applies execution-time placeholder injection to an expression tree. */
    [[nodiscard]] Node* var_injector(Node* node, var_injector_context* injector_context) {
        var_injector_mutator mutator;
        return mutate_pg_expression(node, *injector_context, mutator);
    }

    /**
     * @brief Updates all installed placeholder Const nodes from one match result.
     *
     * Per-tuple update of every placeholder Const* from a match_result's
     * fields. Fields we don't have a Const for (because the query didn't
     * project them) are skipped thanks to the nullptr guard. The cigar text
     * Datum is allocated via CStringGetTextDatum in CurrentMemoryContext.
     */
    void populate_placeholder_consts(placeholder_constants& consts,
                                     const bioseq::simd::match_result& match) {
        using bioseq::simd::format_cigar;
        // Absolute coordinates and lengths are unsigned 64-bit values. They are projected
        // through BIGINT (int8) placeholders so positions / lengths in sequences
        // longer than `INT32_MAX` (~2.15 Gbp) are NOT truncated. `set_bigint`
        // fills those eight (query_len … block_length) via `Int64GetDatum`. The
        // remaining match_result fields (dp_score, mapq, chain_score, seed_count,
        // …) are genuinely `int32_t` in the struct, so they keep INT4 placeholders
        // and `set_int` — widening them at this boundary cannot recover precision
        // the source type never held.
        //
        // `set_bigint`'s Assert is a debug-time witness that no real coordinate
        // reaches the int64 sign boundary: the project's pessimistic ceiling is
        // 1 Tbp (~2^40), ~23 bits below INT64_MAX, so the uint64→int64 reinterpret
        // is always value-preserving.
        // Stores a checked match value in an INT4 placeholder when one was installed.
        auto set_int = [](Const* node, int64_t value) {
            if (node == nullptr)
                return;
            Assert(value >= std::numeric_limits<std::int32_t>::min() &&
                   value <= std::numeric_limits<std::int32_t>::max());
            node->constvalue =
                bioseq::postgres::support::pg_datum::from(static_cast<std::int32_t>(value)).raw();
            node->constisnull = false;
        };
        // Parameter is the underlying type of `sequence_size_t` (std::uint64_t)
        // spelled directly — this TU does not import bioseq_storage_size, so the
        // `sequence_size_t` alias is not in scope here.
        // Stores an unsigned coordinate in an INT8 placeholder after checking its signed range.
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
        // The eight sequence_size_t (uint64) coordinates/lengths → BIGINT, no narrow.
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
            std::string cigar_str = format_cigar(match.cigar);
            consts.cigar_node->constvalue =
                bioseq::postgres::support::pg_datum::from_text_cstring(cigar_str.c_str()).raw();
            consts.cigar_node->constisnull = false;
        }
    }

} // export custom-scan partition declarations
