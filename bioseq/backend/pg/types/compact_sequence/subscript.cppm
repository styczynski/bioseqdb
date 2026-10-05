/**
 * @file subscript.cppm
 * @brief Read sequence letters and slices with zero-based SQL subscripts.
 */
module;

#include <cstdint>
#include <exception>
#include <optional>

#include <bioseq/pg_compat.h>
extern "C" {
#include "catalog/pg_type_d.h"
#include "executor/execExpr.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "nodes/subscripting.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_node.h"
#include "utils/expandeddatum.h"
}

export module bioseq.postgres.types.compact_sequence:subscript;

import bioseq.codegen.sql.annotation_model;
import :flat_abi;
import :tree_types;
import :view_session;
import :packed_output;
import :letter;
import bioseq.postgres.support;
import bioseq.storage.coordinates;

/**
 * @brief Read the sequence length from its header.
 * @param[in] input Non-null sequence Datum valid for this PostgreSQL call.
 * @return Number of letters in the sequence.
 * @note A locator needs  its owning table scan to find the sequence. A bare locator raises an error here.
 */
export [[nodiscard]] inline std::uint64_t sequence_logical_length(Datum input)
{
    const auto descriptor = classify_and_describe_sequence_header(input);
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot resolve a bioseq locator outside its owning table scan"),
                 errdetail("relation, column, snapshot, and cache ownership are unavailable")));
    }
    return descriptor.bp_length;
}

/**
 * @brief Read the letter at a valid sequence index.
 * @param[in] input Non-null sequence Datum vwlid for this PostgreSQL call.
 * @param[in] normalized_index Zero-based index within the sequence.
 * @return Alphabet code and rank of the selected letter.
 */
export [[nodiscard]] inline pg_sequence_rank_value sequence_letter_at(
    Datum input,
    std::uint64_t normalized_index)
{
    return read_pg_sequence_rank_at(input, normalized_index);
}

namespace {

/** @brief Keep evaluated indexes and slice bounds between executor callbacks. */
struct seq_subscript_workspace {
    bool is_slice;
    bool lower_provided;
    bool upper_provided;
    std::int64_t lower;
    std::int64_t upper;
};

/**
 * @brief Convert a subscript expression to PostgreSQL `bigint`.
 * @param[in] pstate Parser state for expression handling and error locations.
 * @param[in] raw Bound expression, or `nullptr` when  the bound was omitted.
 * @return Expression owned by the parser, or `nullptr` for an omitted bound.
 */
[[nodiscard]] Expr* coerce_seq_subscript(ParseState* pstate, Node* raw)
{
    if (raw == nullptr) {
        return nullptr;
    }
    auto* expression = transformExpr(
        pstate, raw, pstate->p_expr_kind);
    auto* coerced = coerce_to_target_type(
        pstate, expression, exprType(expression), INT8OID, -1,
        COERCION_ASSIGNMENT, COERCE_IMPLICIT_CAST, -1);
    if (coerced == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("bioseq.seq subscript must have type bigint"),
                 parser_errposition(pstate, exprLocation(expression))));
    }
    return reinterpret_cast<Expr*>(coerced);
}

/**
 * @brief Parse a sequence subscript and choose its result type.
 * A scalar index returns a letter. A slice keeps the sequence's alphabet.
 * Assignment and multiple dimensions raise errors.
 * @param[in,out] sbsref Parse node receiving the result type and bounds.
 * @param[in] indirection Index, or slice descriptor.
 * @param[in] pstate Parser state for converting bounds to bigint.
 * @param[in] is_slice Request a half-open slice
 * @param[in] is_assignment Request assignment.
 */
void seq_subscript_transform(
    SubscriptingRef* sbsref,
    List* indirection,
    ParseState* pstate,
    bool is_slice,
    bool is_assignment)
{
    const int32_t container_typmod = sbsref->reftypmod;
    if (is_assignment || sbsref->refassgnexpr != nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("assignment to a bioseq.seq subscript is not supported")));
    }
    if (list_length(indirection) != 1) {
        ereport(ERROR,
                (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                 errmsg("bioseq.seq supports exactly one subscript dimension")));
    }
    auto* indices = castNode(A_Indices, linitial(indirection));
    if (is_slice != indices->is_slice) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("inconsistent bioseq slice parse state")));
    }
    sbsref->refupperindexpr = lappend(
        sbsref->refupperindexpr,
        coerce_seq_subscript(pstate, indices->uidx));
    if (is_slice) {
        sbsref->reflowerindexpr = lappend(
            sbsref->reflowerindexpr,
            coerce_seq_subscript(pstate, indices->lidx));
        sbsref->refrestype = sbsref->refcontainertype;
        sbsref->reftypmod = container_typmod;
    } else {
        sbsref->refrestype = bioseq_letter_oid();
        sbsref->reftypmod = letter_typmod_from_seq_typmod(
            container_typmod);
    }
}

/**
 * @brief Save the evaluated bounds, or return SQL NULL for a NULL bound.
 *
 * An omitted slice bound uses the sequence start or end. An  explicitly NULL
 * bound makes the whole subscript result NULL.
 *
 * @param[in,out] op Executor tep holding the bounds and result null flag.
 * @return `true` when the executor should continue to fetch the result.
 */
bool seq_check_subscripts(
    ExprState*,
    ExprEvalStep* op,
    ExprContext*)
{
    auto* state = op->d.sbsref_subscript.state;
    auto* workspace = static_cast<seq_subscript_workspace*>(
        state->workspace);
    workspace->upper_provided = state->upperprovided[0];
    workspace->lower_provided =
        workspace->is_slice && state->lowerprovided[0];
    if (workspace->upper_provided) {
        if (state->upperindexnull[0]) {
            *op->resnull = true;
            return false;
        }
        workspace->upper = DatumGetInt64(state->upperindex[0]);
    }
    if (workspace->lower_provided) {
        if (state->lowerindexnull[0]) {
            *op->resnull = true;
            return false;
        }
        workspace->lower = DatumGetInt64(state->lowerindex[0]);
    }
    return true;
}

/**
 * @brief Fetch one letter or the slice between the requested bounds.
 *
 * Indexes start at zero. Negative indexes count from the end. An element index
 * outside the sequence returns SQL NULL. Slice bounds are clamped to the sequence,
 * and the upper bound is excluded. A slice whose end precedes its start is empty.
 *
 * @param[in,out] op Executor step whose input sequence  is replaced by the result.
 */
inline void seq_fetch_subscript_impl(ExprState*, ExprEvalStep* op, ExprContext*)
{
    auto* state = op->d.sbsref.state;
    auto* workspace = static_cast<seq_subscript_workspace*>(
        state->workspace);
    Assert(!*op->resnull);
    const Datum container = *op->resvalue;
    const std::uint64_t length = sequence_logical_length(container);

    if (!workspace->is_slice) {
        Assert(workspace->upper_provided);
        const auto index = bioseq::storage::normalize_element_index(
            workspace->upper, length);
        if (!index.has_value()) {
            *op->resvalue = Datum{0};
            *op->resnull = true;
            return;
        }
        const auto letter = sequence_letter_at(container, *index);
        *op->resvalue = make_letter_datum(
            letter.alphabet_code, letter.rank);
        *op->resnull = false;
        return;
    }

    const std::uint64_t from = !workspace->lower_provided
        ? 0u : bioseq::storage::normalize_slice_bound(
            workspace->lower, length);
    std::uint64_t to = !workspace->upper_provided
        ? length : bioseq::storage::normalize_slice_bound(
            workspace->upper, length);
    if (to < from) {
        to = from;
    }
    *op->resvalue = slice_pg_sequence(container, from, to);
    *op->resnull = false;
}

/**
 * @brief Fetch a subscript result and translate C++ exceptions to PostgreSQL errors.
 *
 * Reads can carry a PostgreSQL error through C++ cleanup. Rethrow that error
 * after the read owners have been destroyed, preserving its original details.
 * Other C++ exceptions become PostgreSQL internal errors.
 *
 * @param[in] expression Executor expression state passed to the fetch implementation.
 * @param[in,out] op Executor step containing the input and output slots.
 * @param[in] context Executor context passed to the fetch implementation.
 */
inline void seq_fetch_subscript(ExprState* expression,
                                ExprEvalStep* op,
                                ExprContext* context)
{
    ErrorData* exact_error = nullptr;
    bool standard_error = false;
    bool unknown_error = false;
    char detail[512] = "std::exception without detail";
    try {
        seq_fetch_subscript_impl(expression, op, context);
        return;
    } catch (bioseq::postgres::support::postgres_error& error) {
        exact_error = error.release_error_data();
    } catch (std::exception const& error) {
        strlcpy(detail,
                error.what() == nullptr
                    ? "std::exception without detail"
                    : error.what(),
                sizeof(detail));
        standard_error = true;
    } catch (...) {
        unknown_error = true;
    }
    if (exact_error != nullptr) {
        ReThrowError(exact_error);
    }
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq sequence callback raised a C++ exception"),
             errdetail("%s", unknown_error
                     ? "unknown non-standard C++ exception"
                     : (standard_error ? detail
                                       : "empty C++ exception state"))));
    pg_unreachable();
}

/**
 * @brief Allocate subscript workspace and register the  read callbacks.
 * @param[in] sbsref Planned subscript expression.
 * @param[in,out] sbsrefstate Executor state owning the workspace for this expression
 * @param[out] methods Callback table used by PostgreSQL expression evaluation.
 */
void seq_subscript_exec_setup(
    SubscriptingRef const* sbsref,
    SubscriptingRefState* sbsrefstate,
    SubscriptExecSteps* methods)
{
    if (sbsref->refassgnexpr != nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("assignment to bioseq.seq is not supported")));
    }
    auto* workspace = static_cast<seq_subscript_workspace*>(
        palloc0(sizeof(seq_subscript_workspace)));
    workspace->is_slice = sbsref->reflowerindexpr != NIL;
    sbsrefstate->workspace = workspace;
    methods->sbs_check_subscripts = seq_check_subscripts;
    methods->sbs_fetch = seq_fetch_subscript;
    methods->sbs_assign = nullptr;
    methods->sbs_fetch_old = nullptr;
}

} // namespace

export namespace bioseq::pg_types::sql_routines {

/**
 * @brief Return the callback table PostgreSQL uses for `bioseq.seq` subscripts.
 * @param[in] input Unused PostgreSQL internal handler argment.
 * @return Static, read-only callback table, valid until the process exits.
 *
 * ```sql
 * -- PostgreSQL invokes this handler for sequence element and slice subscripts. SQL subscripts are one-based.
 * SELECT ('ACGT'::bioseq.seq('dna4', 64))[2] AS base, ('ACGT'::bioseq.seq('dna4', 64))[2:3] AS slice;
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::type_subscript({
    .for_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::internal,
})]]
void* seq_subscript_handler(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::pg::internal,
    })]]
    std::optional<bioseq::postgres::support::subscript_handler> input)
{
    static_cast<void>(input);
    static const SubscriptRoutines routines = {
        seq_subscript_transform,
        seq_subscript_exec_setup,
        true,
        false,
        false,
    };
    return const_cast<SubscriptRoutines*>(&routines);
}

}  // namespace bioseq::pg_types::sql_routines
