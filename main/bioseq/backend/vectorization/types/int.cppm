/*-------------------------------------------------------------------------
 *
 * vectorization/types/int.cppm
 *
 * Instantiates vectorised comparison operators for PostgreSQL's integer
 * types (char, int2, int4, int8) and the cross-width combinations used in
 * SQL (int24, int28, int42, int48, int82, int84). Each macro expansion
 * emits a pair of CXX_PG_FUNCTION_INFO_V1 declarations for a single SQL
 * operator implementation.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include "columnar/vectorization/types/types.h"

export module bioseq_vector_types_int;

extern "C" {

// char
BUILD_CMP_OPERATOR_INT( char,  char,  char)

// int2
BUILD_CMP_OPERATOR_INT( int2, int16, int16)
BUILD_CMP_OPERATOR_INT(int24, int16, int32)
BUILD_CMP_OPERATOR_INT(int28, int16, int64)

// int4
BUILD_CMP_OPERATOR_INT( int4, int32, int32)
BUILD_CMP_OPERATOR_INT(int42, int32, int16)
BUILD_CMP_OPERATOR_INT(int48, int32, int64)

// int8
BUILD_CMP_OPERATOR_INT( int8, int64, int64)
BUILD_CMP_OPERATOR_INT(int82, int64, int16)
BUILD_CMP_OPERATOR_INT(int84, int64, int32)

}
