/**
 * @file subscript.cppm
 * @brief PostgreSQL subscript handling for zero-based half-open sequence access.
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
 * @brief Reads the logical base-pair length from an inline or tree sequence Datum.
 * @param[in] input Non-null sequence Datum valid for the current PostgreSQL call.
 * @return Logical length in base pairs.
 * @note Locator Datums require scan ownership and therefore report a PostgreSQL error here.
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
 * @brief Reads one normalized base rank from a sequence Datum.
 * @param[in] input Non-null sequence Datum valid for the current PostgreSQL call.
 * @param[in] normalized_index Zero-based index known to be within the logical length.
 * @return Alphabet code and rank for the selected base.
 */
export [[nodiscard]] inline pg_sequence_rank_value sequence_letter_at(
    Datum input,
    std::uint64_t normalized_index)
{
    return read_pg_sequence_rank_at(input, normalized_index);
}

namespace {

/** @brief Carries normalized scalar or slice bounds between executor callbacks. */
struct seq_subscript_workspace {
    bool is_slice;
    bool lower_provided;
    bool upper_provided;
    std::int64_t lower;
    std::int64_t upper;
};

/**
 * @brief Coerces one optional subscript expression to PostgreSQL `bigint`.
 * @param[in] pstate Parser state used for transformation and error positioning.
 * @param[in] raw Raw bound expression, or `nullptr` for an omitted slice bound.
 * @return Parser-owned coerced expression, or `nullptr` for an omitted bound.
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
 * @brief Validates a one-dimensional read subscript and installs coerced bounds.
 * @param[in,out] sbsref Parse node populated with result and bound expressions.
 * @param[in] indirection Single PostgreSQL index or slice descriptor.
 * @param[in] pstate Parser state used to transform bound expressions.
 * @param[in] is_slice Whether the expression requests a half-open slice.
 * @param[in] is_assignment Whether the parser requested unsupported assignment semantics.
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
 * @brief Copies evaluated bounds into workspace and propagates SQL null bounds.
 * @param[in,out] op Executor step containing evaluated bound values and result null state.
 * @return `true` when fetch evaluation should continue.
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
 * @brief Produces a nullable letter or a non-null half-open sequence slice.
 * @param[in,out] op Executor step whose input Datum and result slot are reused in place.
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
 * @brief Executes subscript fetches without allowing C++ exceptions to cross PostgreSQL.
 *
 * Source reads may surface PostgreSQL errors long enough to unwind read-session owners.
 * This callback transfers the exact error back only after fetch-local C++ objects die.
 *
 * @param[in] expression Executor expression state forwarded to the implementation.
 * @param[in,out] op Executor step containing the input and output slots.
 * @param[in] context Executor context forwarded to the implementation.
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
 * @brief Allocates expression-lifetime workspace and installs read-only callbacks.
 * @param[in] sbsref Planned subscript expression.
 * @param[in,out] sbsrefstate Executor state that owns the allocated workspace.
 * @param[out] methods Callback table consumed by PostgreSQL expression evaluation.
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
 * @brief Returns the process-lifetime callback table for `bioseq.seq` subscripting.
 * @param[in] input PostgreSQL internal handler argument, unused by this type.
 * @return Static read-only subscript routine table exposed as PostgreSQL `internal`.
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
