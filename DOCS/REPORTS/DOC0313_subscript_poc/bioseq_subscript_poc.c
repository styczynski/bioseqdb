/*
 * bioseq_subscript_poc.c
 *
 * Throwaway proof-of-concept: a PostgreSQL "generic subscripting" handler
 * (PG14+ SUBSCRIPT API) attached to the EXISTING `bioseq.seq` type WITHOUT
 * recompiling the heavy bioseq C++ module.
 *
 * It proves the native bracket grammar works on a non-array custom varlena:
 *     s[i]        element  -> bioseq.seq (1 letter)  (NULL if out of range)
 *     s[lo:hi]    slice    -> bioseq.seq
 *     s[:hi]      prefix   -> bioseq.seq
 *     s[lo:]      suffix   -> bioseq.seq
 * Indices are 1-based and inclusive (PostgreSQL array convention).
 *
 * The fetch path is intentionally "dirty": it decodes the whole sequence to
 * ASCII via the type's own seq_out, slices the ASCII, and re-encodes via
 * seq_in. That reuses the battle-tested codec for ALL alphabets/widths and
 * keeps the PoC tiny. A production handler would do O(1)/O(slice) bit-level
 * extraction instead (see the report).
 */
#include "postgres.h"
#include "fmgr.h"
#include "nodes/subscripting.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "nodes/nodeFuncs.h"
#include "executor/execExpr.h"
#include "parser/parse_node.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "utils/lsyscache.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include <string.h>

PG_MODULE_MAGIC;

/* Per-expression workspace stashed in SubscriptingRefState->workspace. */
typedef struct BioseqSubWorkspace
{
	bool	is_slice;		/* slice vs single-element */
	Oid		container_type; /* bioseq.seq oid */
	Oid		typoutput;		/* seq_out */
	bool	typisvarlena;
	Oid		typinput;		/* seq_in */
	Oid		typioparam;		/* I/O param oid for seq_in */
} BioseqSubWorkspace;

/* ---- parse-analysis: transform raw subscripts into upper/lower index lists */
static void
bioseq_seq_subscript_transform(SubscriptingRef *sbsref,
							   List *indirection,
							   ParseState *pstate,
							   bool isSlice,
							   bool isAssignment)
{
	List	   *upperIndexpr = NIL;
	List	   *lowerIndexpr = NIL;
	ListCell   *idx;

	if (isAssignment)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("bioseq.seq does not support subscripted assignment (PoC)")));

	foreach(idx, indirection)
	{
		A_Indices  *ai = lfirst_node(A_Indices, idx);
		Node	   *subexpr;

		if (isSlice)
		{
			if (ai->lidx)
			{
				subexpr = transformExpr(pstate, ai->lidx, pstate->p_expr_kind);
				subexpr = coerce_to_target_type(pstate, subexpr, exprType(subexpr),
												INT4OID, -1, COERCION_ASSIGNMENT,
												COERCE_IMPLICIT_CAST, -1);
				if (subexpr == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("bioseq.seq subscript lower bound must be integer")));
			}
			else
				subexpr = NULL; /* omitted lower bound, e.g. s[:hi] */
			lowerIndexpr = lappend(lowerIndexpr, subexpr);
		}
		else
			Assert(ai->lidx == NULL && !ai->is_slice);

		if (ai->uidx)
		{
			subexpr = transformExpr(pstate, ai->uidx, pstate->p_expr_kind);
			subexpr = coerce_to_target_type(pstate, subexpr, exprType(subexpr),
											INT4OID, -1, COERCION_ASSIGNMENT,
											COERCE_IMPLICIT_CAST, -1);
			if (subexpr == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("bioseq.seq subscript must be integer")));
		}
		else
		{
			Assert(isSlice);
			subexpr = NULL;		/* omitted upper bound, e.g. s[lo:] */
		}
		upperIndexpr = lappend(upperIndexpr, subexpr);
	}

	sbsref->refupperindexpr = upperIndexpr;
	sbsref->reflowerindexpr = lowerIndexpr;

	/* PoC: element AND slice both return the container type (bioseq.seq). */
	sbsref->refrestype = sbsref->refcontainertype;
	/* keep the same typmod (alphabet/width) on the result */
	/* sbsref->reftypmod already describes the container; leave as-is */
}

/* ---- derive a bioseq typmod from the raw varlena header bytes.
 * Layout (little-endian, 4-byte varlena header):
 *   [0..3] vl_len_   [4] alphabet_code   [5] storage_unit_byte_width(bit7=stub)
 * typmod = (alphabet_code << 8) | log2(byte_width).
 */
static int32
bioseq_typmod_from_header(struct varlena *raw)
{
	const unsigned char *b = (const unsigned char *) raw;
	unsigned char alpha = b[4];
	unsigned char subw = (unsigned char) (b[5] & 0x0F);
	int width_code = (subw == 1) ? 0 : (subw == 2) ? 1 : (subw == 4) ? 2 : 3;
	return (int32) ((((int32) alpha) << 8) | width_code);
}

/* ---- executor: subscript validation (reject NULL subscripts under strict) */
static bool
bioseq_seq_check_subscripts(ExprState *state, ExprEvalStep *op,
							ExprContext *econtext)
{
	SubscriptingRefState *sbs = op->d.sbsref_subscript.state;
	int i;

	for (i = 0; i < sbs->numupper; i++)
		if (sbs->upperprovided[i] && sbs->upperindexnull[i])
			return false;
	for (i = 0; i < sbs->numlower; i++)
		if (sbs->lowerprovided[i] && sbs->lowerindexnull[i])
			return false;
	return true;
}

/* ---- executor: the fetch itself */
static void
bioseq_seq_fetch(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	SubscriptingRefState *sbs = op->d.sbsref.state;
	BioseqSubWorkspace *ws = (BioseqSubWorkspace *) sbs->workspace;
	struct varlena *raw;
	int32		typmod;
	char	   *ascii;
	int			len;
	long		lo,
				hi;
	int			outlen;
	char	   *slice;
	Datum		result;

	if (*op->resnull)
		return;					/* strict: NULL container -> NULL result */

	raw = (struct varlena *) PG_DETOAST_DATUM(*op->resvalue);
	typmod = bioseq_typmod_from_header(raw);

	ascii = OidOutputFunctionCall(ws->typoutput, PointerGetDatum(raw));
	len = (int) strlen(ascii);

	if (ws->is_slice)
	{
		lo = sbs->lowerprovided[0] ? (long) DatumGetInt32(sbs->lowerindex[0]) : 1;
		hi = sbs->upperprovided[0] ? (long) DatumGetInt32(sbs->upperindex[0]) : (long) len;
	}
	else
	{
		lo = hi = (long) DatumGetInt32(sbs->upperindex[0]);
		/* array-style: out-of-range single element -> NULL */
		if (lo < 1 || lo > len)
		{
			*op->resnull = true;
			*op->resvalue = (Datum) 0;
			return;
		}
	}

	if (lo < 1)
		lo = 1;
	if (hi > len)
		hi = len;
	outlen = (hi >= lo) ? (int) (hi - lo + 1) : 0;

	slice = palloc(outlen + 1);
	if (outlen > 0)
		memcpy(slice, ascii + (lo - 1), outlen);
	slice[outlen] = '\0';

	result = OidInputFunctionCall(ws->typinput, slice, ws->typioparam, typmod);
	*op->resvalue = result;
	*op->resnull = false;
}

/* ---- executor-startup compilation: install the step methods + workspace */
static void
bioseq_seq_exec_setup(const SubscriptingRef *sbsref,
					  SubscriptingRefState *sbsrefstate,
					  SubscriptExecSteps *methods)
{
	BioseqSubWorkspace *ws = (BioseqSubWorkspace *) palloc0(sizeof(BioseqSubWorkspace));

	ws->is_slice = (sbsrefstate->numlower != 0);
	ws->container_type = sbsref->refcontainertype;
	getTypeOutputInfo(sbsref->refcontainertype, &ws->typoutput, &ws->typisvarlena);
	getTypeInputInfo(sbsref->refrestype, &ws->typinput, &ws->typioparam);
	sbsrefstate->workspace = ws;

	methods->sbs_check_subscripts = bioseq_seq_check_subscripts;
	methods->sbs_fetch = bioseq_seq_fetch;
	methods->sbs_assign = NULL;
	methods->sbs_fetch_old = NULL;
}

/* ---- the SQL-visible handler: returns a pointer to the routines table */
PG_FUNCTION_INFO_V1(bioseq_seq_subscript_handler);
Datum
bioseq_seq_subscript_handler(PG_FUNCTION_ARGS)
{
	static const SubscriptRoutines sbsroutines = {
		.transform = bioseq_seq_subscript_transform,
		.exec_setup = bioseq_seq_exec_setup,
		.fetch_strict = true,		/* NULL container/subscript -> NULL */
		.fetch_leakproof = false,	/* re-encode may throw -> not leakproof */
		.store_leakproof = false
	};

	PG_RETURN_POINTER(&sbsroutines);
}
