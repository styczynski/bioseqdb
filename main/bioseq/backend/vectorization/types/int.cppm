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

import pg_utils;

extern "C" {

// char
BUILD_CMP_OPERATOR_INT( char,  char,  char)

// int2
BUILD_CMP_OPERATOR_INT( int2, int16_t, int16_t)
BUILD_CMP_OPERATOR_INT(int24, int16_t, int32_t)
BUILD_CMP_OPERATOR_INT(int28, int16_t, int64_t)

// int4
BUILD_CMP_OPERATOR_INT( int4, int32_t, int32_t)
BUILD_CMP_OPERATOR_INT(int42, int32_t, int16_t)
BUILD_CMP_OPERATOR_INT(int48, int32_t, int64_t)

// int8
BUILD_CMP_OPERATOR_INT( int8, int64_t, int64_t)
BUILD_CMP_OPERATOR_INT(int82, int64_t, int16_t)
BUILD_CMP_OPERATOR_INT(int84, int64_t, int32_t)

}
