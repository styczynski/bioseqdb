/* PostgreSQL 17 custom subscript handler for zero-based half-open seq access. */
module;

#include <cstdint>

#include "headers/pg_compat.h"
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

export module bioseq.types.pg_compact_sequence:subscript;

import :flat_abi;
import :tree_types;
import :view_session;
import :packed_output;
import :letter;
import bioseq.storage.coordinates;

export [[nodiscard]] inline std::uint64_t sequence_logical_length(Datum input)
{
    Datum dispatch = input;
    pgseq_t* flat = nullptr;
    if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
        flat = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(input));
        dispatch = PointerGetDatum(flat);
    }
    const auto descriptor = classify_and_describe_sequence(dispatch);
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot resolve a bioseq locator outside its owning table scan"),
                 errdetail("relation, column, snapshot, and cache ownership are unavailable")));
    }
    if (flat != nullptr
        && reinterpret_cast<Pointer>(flat) != DatumGetPointer(input)) {
        pfree(flat);
    }
    return descriptor.bp_length;
}

export [[nodiscard]] inline pg_sequence_rank_value sequence_letter_at(
    Datum input,
    std::uint64_t normalized_index)
{
    return read_pg_sequence_rank_at(input, normalized_index);
}

namespace {

struct seq_subscript_workspace {
    bool is_slice;
    bool lower_provided;
    bool upper_provided;
    std::int64_t lower;
    std::int64_t upper;
};

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

void seq_fetch_subscript(ExprState*, ExprEvalStep* op, ExprContext*)
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

export inline Datum seq_subscript_handler_impl(FunctionCallInfo)
{
    static const SubscriptRoutines routines = {
        seq_subscript_transform,
        seq_subscript_exec_setup,
        true,
        false,
        false,
    };
    PG_RETURN_POINTER(&routines);
}
