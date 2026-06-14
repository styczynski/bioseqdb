/*-------------------------------------------------------------------------
 *
 * vectorization/types/int.cppm
 *
 * Thin per-symbol wrappers for the vectorised comparison operators on
 * PostgreSQL's integer types (char, int2, int4, int8) and the cross-width
 * combinations used in SQL (int24, int28, int42, int48, int82, int84). Each
 * extern "C" CXX_PG_FUNCTION_INFO_V1 wrapper preserves the exact SQL catalog
 * symbol name (v<name><op>) and forwards to the shared templated marshaller
 * vector_cmp<CmpOp, LT, RT> (the codegen macro it replaced is retired).
 *
 * NB: PostgreSQL's "char" type is a single byte; it is bound to int8_t here so
 * the Highway kernel has a supported lane type (same width + signedness on our
 * targets), preserving the original `char OP char` comparison semantics.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

export module bioseq_vector_types_int;

import pg_utils;
import bioseq_column_vector_compare_pg;   // vector_cmp<CmpOp, LT, RT>

using bioseq::column::vec::CmpOp;

extern "C" {

// char (single-byte, bound to int8_t for HWY)
CXX_PG_FUNCTION_INFO_V1(vchareq); Datum vchareq(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::eq,int8_t,int8_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vcharne); Datum vcharne(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ne,int8_t,int8_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vchargt); Datum vchargt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::gt,int8_t,int8_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vcharlt); Datum vcharlt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::lt,int8_t,int8_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vcharle); Datum vcharle(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::le,int8_t,int8_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vcharge); Datum vcharge(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ge,int8_t,int8_t>(fcinfo);}

// int2 (int16)
CXX_PG_FUNCTION_INFO_V1(vint2eq); Datum vint2eq(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::eq,int16_t,int16_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint2ne); Datum vint2ne(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ne,int16_t,int16_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint2gt); Datum vint2gt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::gt,int16_t,int16_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint2lt); Datum vint2lt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::lt,int16_t,int16_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint2le); Datum vint2le(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::le,int16_t,int16_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint2ge); Datum vint2ge(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ge,int16_t,int16_t>(fcinfo);}

// int24 (int16 column, int32 const)
CXX_PG_FUNCTION_INFO_V1(vint24eq); Datum vint24eq(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::eq,int16_t,int32_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint24ne); Datum vint24ne(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ne,int16_t,int32_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint24gt); Datum vint24gt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::gt,int16_t,int32_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint24lt); Datum vint24lt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::lt,int16_t,int32_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint24le); Datum vint24le(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::le,int16_t,int32_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint24ge); Datum vint24ge(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ge,int16_t,int32_t>(fcinfo);}

// int28 (int16 column, int64 const)
CXX_PG_FUNCTION_INFO_V1(vint28eq); Datum vint28eq(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::eq,int16_t,int64_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint28ne); Datum vint28ne(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ne,int16_t,int64_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint28gt); Datum vint28gt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::gt,int16_t,int64_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint28lt); Datum vint28lt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::lt,int16_t,int64_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint28le); Datum vint28le(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::le,int16_t,int64_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint28ge); Datum vint28ge(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ge,int16_t,int64_t>(fcinfo);}

// int4 (int32)
CXX_PG_FUNCTION_INFO_V1(vint4eq); Datum vint4eq(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::eq,int32_t,int32_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint4ne); Datum vint4ne(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ne,int32_t,int32_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint4gt); Datum vint4gt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::gt,int32_t,int32_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint4lt); Datum vint4lt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::lt,int32_t,int32_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint4le); Datum vint4le(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::le,int32_t,int32_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint4ge); Datum vint4ge(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ge,int32_t,int32_t>(fcinfo);}

// int42 (int32 column, int16 const)
CXX_PG_FUNCTION_INFO_V1(vint42eq); Datum vint42eq(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::eq,int32_t,int16_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint42ne); Datum vint42ne(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ne,int32_t,int16_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint42gt); Datum vint42gt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::gt,int32_t,int16_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint42lt); Datum vint42lt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::lt,int32_t,int16_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint42le); Datum vint42le(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::le,int32_t,int16_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint42ge); Datum vint42ge(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ge,int32_t,int16_t>(fcinfo);}

// int48 (int32 column, int64 const)
CXX_PG_FUNCTION_INFO_V1(vint48eq); Datum vint48eq(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::eq,int32_t,int64_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint48ne); Datum vint48ne(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ne,int32_t,int64_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint48gt); Datum vint48gt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::gt,int32_t,int64_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint48lt); Datum vint48lt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::lt,int32_t,int64_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint48le); Datum vint48le(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::le,int32_t,int64_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint48ge); Datum vint48ge(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ge,int32_t,int64_t>(fcinfo);}

// int8 (int64)
CXX_PG_FUNCTION_INFO_V1(vint8eq); Datum vint8eq(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::eq,int64_t,int64_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint8ne); Datum vint8ne(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ne,int64_t,int64_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint8gt); Datum vint8gt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::gt,int64_t,int64_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint8lt); Datum vint8lt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::lt,int64_t,int64_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint8le); Datum vint8le(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::le,int64_t,int64_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint8ge); Datum vint8ge(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ge,int64_t,int64_t>(fcinfo);}

// int82 (int64 column, int16 const)
CXX_PG_FUNCTION_INFO_V1(vint82eq); Datum vint82eq(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::eq,int64_t,int16_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint82ne); Datum vint82ne(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ne,int64_t,int16_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint82gt); Datum vint82gt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::gt,int64_t,int16_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint82lt); Datum vint82lt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::lt,int64_t,int16_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint82le); Datum vint82le(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::le,int64_t,int16_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint82ge); Datum vint82ge(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ge,int64_t,int16_t>(fcinfo);}

// int84 (int64 column, int32 const)
CXX_PG_FUNCTION_INFO_V1(vint84eq); Datum vint84eq(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::eq,int64_t,int32_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint84ne); Datum vint84ne(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ne,int64_t,int32_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint84gt); Datum vint84gt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::gt,int64_t,int32_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint84lt); Datum vint84lt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::lt,int64_t,int32_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint84le); Datum vint84le(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::le,int64_t,int32_t>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vint84ge); Datum vint84ge(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ge,int64_t,int32_t>(fcinfo);}

}
