/*-------------------------------------------------------------------------
 *
 * vectorization/types/date.cppm
 *
 * Instantiates vectorised comparison operators for PostgreSQL's temporal
 * types (date, time, timestamp, timestamptz) via the BUILD_CMP_OPERATOR_INT
 * macro. The macro emits a CXX_PG_FUNCTION_INFO_V1 pair for each operator
 * so the generated symbols are resolvable from SQL.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include "columnar/vectorization/types/types.h"

export module bioseq_vector_types_date;

extern "C" {

// date (int32)
BUILD_CMP_OPERATOR_INT(date_, DateADT, DateADT)

// time (int64)
BUILD_CMP_OPERATOR_INT(time_, TimeADT, TimeADT)

// timestamp (int64)
BUILD_CMP_OPERATOR_INT(timestamp_, Timestamp, Timestamp)

// timestamptz (int64)
BUILD_CMP_OPERATOR_INT(timestamptz_, TimestampTz, TimestampTz)

}
