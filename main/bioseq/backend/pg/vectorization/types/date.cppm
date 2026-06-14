/*-------------------------------------------------------------------------
 *
 * vectorization/types/date.cppm
 *
 * Thin per-symbol wrappers for the vectorised comparison operators on
 * PostgreSQL's temporal types (date, time, timestamp, timestamptz). Each
 * extern "C" CXX_PG_FUNCTION_INFO_V1 wrapper preserves the exact SQL catalog
 * symbol name (v<name><op>) and forwards to the shared templated marshaller
 * vector_cmp<CmpOp, LT, RT> (the codegen macro it replaced is retired).
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

export module bioseq_vector_types_date;

import pg_utils;
import bioseq_column_vector_compare_pg;   // vector_cmp<CmpOp, LT, RT>

using bioseq::column::vec::CmpOp;

extern "C" {

// date (int32)
CXX_PG_FUNCTION_INFO_V1(vdate_eq); Datum vdate_eq(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::eq,DateADT,DateADT>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vdate_ne); Datum vdate_ne(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ne,DateADT,DateADT>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vdate_gt); Datum vdate_gt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::gt,DateADT,DateADT>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vdate_lt); Datum vdate_lt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::lt,DateADT,DateADT>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vdate_le); Datum vdate_le(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::le,DateADT,DateADT>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vdate_ge); Datum vdate_ge(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ge,DateADT,DateADT>(fcinfo);}

// time (int64)
CXX_PG_FUNCTION_INFO_V1(vtime_eq); Datum vtime_eq(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::eq,TimeADT,TimeADT>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vtime_ne); Datum vtime_ne(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ne,TimeADT,TimeADT>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vtime_gt); Datum vtime_gt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::gt,TimeADT,TimeADT>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vtime_lt); Datum vtime_lt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::lt,TimeADT,TimeADT>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vtime_le); Datum vtime_le(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::le,TimeADT,TimeADT>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vtime_ge); Datum vtime_ge(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ge,TimeADT,TimeADT>(fcinfo);}

// timestamp (int64)
CXX_PG_FUNCTION_INFO_V1(vtimestamp_eq); Datum vtimestamp_eq(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::eq,Timestamp,Timestamp>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vtimestamp_ne); Datum vtimestamp_ne(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ne,Timestamp,Timestamp>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vtimestamp_gt); Datum vtimestamp_gt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::gt,Timestamp,Timestamp>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vtimestamp_lt); Datum vtimestamp_lt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::lt,Timestamp,Timestamp>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vtimestamp_le); Datum vtimestamp_le(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::le,Timestamp,Timestamp>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vtimestamp_ge); Datum vtimestamp_ge(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ge,Timestamp,Timestamp>(fcinfo);}

// timestamptz (int64)
CXX_PG_FUNCTION_INFO_V1(vtimestamptz_eq); Datum vtimestamptz_eq(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::eq,TimestampTz,TimestampTz>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vtimestamptz_ne); Datum vtimestamptz_ne(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ne,TimestampTz,TimestampTz>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vtimestamptz_gt); Datum vtimestamptz_gt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::gt,TimestampTz,TimestampTz>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vtimestamptz_lt); Datum vtimestamptz_lt(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::lt,TimestampTz,TimestampTz>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vtimestamptz_le); Datum vtimestamptz_le(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::le,TimestampTz,TimestampTz>(fcinfo);}
CXX_PG_FUNCTION_INFO_V1(vtimestamptz_ge); Datum vtimestamptz_ge(PG_FUNCTION_ARGS){ return vector_cmp<CmpOp::ge,TimestampTz,TimestampTz>(fcinfo);}

}
