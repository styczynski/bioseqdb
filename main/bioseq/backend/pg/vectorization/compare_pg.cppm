/*-------------------------------------------------------------------------
 *
 * vectorization/compare_pg.cppm   (module bioseq_column_vector_compare_pg)
 *
 * Domain-P binding for the Domain-N comparison kernel. Supplies the concrete
 * VectorColumn-backed result sink that SATISFIES bioseq::column::vec::
 * is_compare_result_sink, plus the templated per-operator marshaller
 * (vector_cmp) that the 84 SQL comparison UDFs in types/int.cppm and
 * types/date.cppm call. This is the "Postgres satisfies the concept" side of
 * the boundary; it ends with a static_assert that locks the concept.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <span>
#include <utility>

export module bioseq_column_vector_compare_pg;

import bioseq_vector_types;          // VectorColumn, BuildVectorColumn, VectorFnArgument
export import bioseq_column.vector_compare; // the Domain-N kernel + concept (re-export so the
                                            // int.cppm/date.cppm wrappers see CmpOp + the kernel)
import pg_utils;

using namespace bioseq::column::vec;

/*
 * The concrete sink that SATISFIES is_compare_result_sink: allocates a bool
 * result column via the existing BuildVectorColumn (palloc) and hands the
 * kernel its two writable spans.
 */
export struct vector_column_result_sink {
	VectorColumn* res = nullptr;
	// allocate() sizes the result column ONCE per batch; value_data()/null_data()
	// then hand the kernel the raw writable bool bases. We deliberately return
	// plain `bool*` (never a span/pair by value): an aggregate return across this
	// Domain-P -> Domain-N module boundary is mis-marshalled by clang's modules
	// ABI and shifts the kernel's write pointer (DOC0322 §6.4).
	void allocate(std::uint32_t n) {
		res = BuildVectorColumn(n, /*typeLen=*/1, /*isVal=*/true, /*rowNumber=*/nullptr);
		res->dimension = n;
	}
	bool* value_data() { return reinterpret_cast<bool*>(res->value); }
	bool* null_data()  { return res->isnull; }
};
static_assert(is_compare_result_sink<vector_column_result_sink>);  // boundary lock (LOAD-BEARING)

/*
 * Shared templated marshaller: collapses the 84 macro-expanded
 * _BUILD_CMP_OP_INT bodies into ONE instantiable kernel call. LT is the
 * left/column-value type, RT is the right/constant type (matching the original
 * macro's LTYPE/RTYPE). The two branches pass swapped <ValueT,ConstT> to the
 * kernel, reproducing the macro's cross-width behaviour.
 */
export template <CmpOp Op, class LT, class RT>
[[gnu::always_inline]] inline Datum vector_cmp(FunctionCallInfo fcinfo) {
	auto* left  = reinterpret_cast<VectorFnArgument*>(PG_GETARG_POINTER(0));
	auto* right = reinterpret_cast<VectorFnArgument*>(PG_GETARG_POINTER(1));
	vector_column_result_sink sink;                 // satisfies is_compare_result_sink

	if (left->type == VECTOR_FN_ARG_VAR && right->type == VECTOR_FN_ARG_CONSTANT) {
		auto* col = reinterpret_cast<VectorColumn*>(left->arg);          // column read as LT
		compare_column<Op, LT, RT>(                                      // ValueT=LT, ConstT=RT
			reinterpret_cast<const LT*>(col->value), col->isnull, col->dimension,
			static_cast<RT>(right->arg), sink);
	} else if (left->type == VECTOR_FN_ARG_CONSTANT && right->type == VECTOR_FN_ARG_VAR) {
		auto* col = reinterpret_cast<VectorColumn*>(right->arg);         // column read as RT
		compare_column<Op, RT, LT>(                                      // ValueT=RT, ConstT=LT
			reinterpret_cast<const RT*>(col->value), col->isnull, col->dimension,
			static_cast<LT>(left->arg), sink);
	}
	// else (CONST/CONST or VAR/VAR) -> sink.res stays null, exactly like the macro's empty else.
	PG_RETURN_POINTER(sink.res);
}
