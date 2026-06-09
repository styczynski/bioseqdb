/*-------------------------------------------------------------------------
 *
 * vectorization/types/aggregates.cppm
 *
 * Vectorised aggregate functions for integer/date types. Each SQL function
 * walks the single VectorColumn argument, skipping null lanes, and folds
 * the lane values into a scalar result. These are the transitions used by
 * sum(), count(), min()/max() and the int8 avg() state machine.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include "columnar/vectorization/types/types.h"
#include "columnar/vectorization/types/numeric.h"

#include <algorithm>

export module bioseq_vector_types_aggregates;

import mlogging;
import pg_utils;

extern "C" {

/* count */

/*
 * vemptycount is the trivial transition for count() when all rows in a
 * batch are guaranteed non-null -- returns the running count unchanged.
 */
CXX_PG_FUNCTION_INFO_V1(vemptycount);
Datum vemptycount(PG_FUNCTION_ARGS)
{
	int64_t running_count = PG_GETARG_INT64(0);
	PG_RETURN_INT64(running_count);
}

/*
 * vanycount is the count() transition that handles nullable inputs. Adds
 * the number of non-null lanes in the VectorColumn to the running count.
 */
CXX_PG_FUNCTION_INFO_V1(vanycount);
Datum
vanycount(PG_FUNCTION_ARGS)
{
	int64_t running_count = PG_GETARG_INT64(0);
	VectorColumn* input_column = reinterpret_cast<VectorColumn*>(PG_GETARG_POINTER(1));

	for (int lane_index = 0; lane_index < input_column->dimension; lane_index++)
	{
		if (input_column->isnull[lane_index])
			continue;

		running_count++;
	}

	PG_RETURN_INT64(running_count);
}

/* int2 */

/*
 * vint2sum folds a VectorColumn of int16 values into a running int64 sum.
 * Null lanes are skipped.
 */
CXX_PG_FUNCTION_INFO_V1(vint2sum);
Datum
vint2sum(PG_FUNCTION_ARGS)
{
	int64_t running_sum = PG_GETARG_INT64(0);
	VectorColumn* input_column = reinterpret_cast<VectorColumn*>(PG_GETARG_POINTER(1));

	int16_t* lane_values = reinterpret_cast<int16_t*>(input_column->value);

	for (int lane_index = 0; lane_index < input_column->dimension; lane_index++)
	{
		if (!input_column->isnull[lane_index])
			running_sum += static_cast<int64_t>(lane_values[lane_index]);
	}

	PG_RETURN_INT64(running_sum);
}

/*
 * vint2acc is the int16 avg() transition. It updates an Int64AggState
 * (N, sumX) held in a PostgreSQL array, potentially copying the array when
 * we're not invoked in an aggregate context.
 */
CXX_PG_FUNCTION_INFO_V1(vint2acc);
Datum
vint2acc(PG_FUNCTION_ARGS)
{
	ArrayType* transition_array;
	VectorColumn* input_column = reinterpret_cast<VectorColumn*>(PG_GETARG_POINTER(1));

	if (AggCheckCallContext(fcinfo, NULL))
		transition_array = PG_GETARG_ARRAYTYPE_P(0);
	else
		transition_array = PG_GETARG_ARRAYTYPE_P_COPY(0);

	Int64AggState* transition_data = reinterpret_cast<Int64AggState*>(ARR_DATA_PTR(transition_array));

	int16_t* lane_values = reinterpret_cast<int16_t*>(input_column->value);

	for (int lane_index = 0; lane_index < input_column->dimension; lane_index++)
	{
		if (!input_column->isnull[lane_index])
		{
			transition_data->N++;
			transition_data->sumX += static_cast<int64_t>(lane_values[lane_index]);
		}
	}

	PG_RETURN_ARRAYTYPE_P(transition_array);
}

/*
 * vint2larger is the int16 max() transition. Replaces maxValue with the
 * largest non-null lane value.
 */
CXX_PG_FUNCTION_INFO_V1(vint2larger);
Datum vint2larger(PG_FUNCTION_ARGS)
{
	int16_t running_max = PG_GETARG_INT16(0);
	VectorColumn* input_column = reinterpret_cast<VectorColumn*>(PG_GETARG_POINTER(1));
	int16_t lane_max = running_max;

	int16_t* lane_values = reinterpret_cast<int16_t*>(input_column->value);

	for (int lane_index = 0; lane_index < input_column->dimension; lane_index++)
	{
		if (input_column->isnull[lane_index])
			continue;

		lane_max = std::max(lane_max, lane_values[lane_index]);
	}

	running_max = std::max(running_max, lane_max);

	PG_RETURN_INT16(running_max);
}

/*
 * vint2smaller is the int16 min() transition.
 */
CXX_PG_FUNCTION_INFO_V1(vint2smaller);
Datum vint2smaller(PG_FUNCTION_ARGS)
{
	int16_t running_min = PG_GETARG_INT32(0);
	VectorColumn* input_column = reinterpret_cast<VectorColumn*>(PG_GETARG_POINTER(1));
	int16_t lane_min = running_min;

	int16_t* lane_values = reinterpret_cast<int16_t*>(input_column->value);

	for (int lane_index = 0; lane_index < input_column->dimension; lane_index++)
	{
		if (input_column->isnull[lane_index])
			continue;

		lane_min = std::min(lane_min, lane_values[lane_index]);
	}

	running_min = std::min(running_min, lane_min);

	PG_RETURN_INT16(running_min);
}

/* int4 */

/*
 * vint4sum folds a VectorColumn of int32 values into a running int64 sum.
 */
CXX_PG_FUNCTION_INFO_V1(vint4sum);
Datum
vint4sum(PG_FUNCTION_ARGS)
{
	int64_t running_sum = PG_GETARG_INT64(0);
	VectorColumn* input_column = reinterpret_cast<VectorColumn*>(PG_GETARG_POINTER(1));

	int32_t* lane_values = reinterpret_cast<int32_t*>(input_column->value);

	for (int lane_index = 0; lane_index < input_column->dimension; lane_index++)
	{
		if (!input_column->isnull[lane_index])
			running_sum += static_cast<int64_t>(lane_values[lane_index]);
	}

	PG_RETURN_INT64(running_sum);
}

/*
 * vint4acc is the int32 avg() transition, identical structure to vint2acc.
 */
CXX_PG_FUNCTION_INFO_V1(vint4acc);
Datum
vint4acc(PG_FUNCTION_ARGS)
{
	ArrayType* transition_array;
	VectorColumn* input_column = reinterpret_cast<VectorColumn*>(PG_GETARG_POINTER(1));

	if (AggCheckCallContext(fcinfo, NULL))
		transition_array = PG_GETARG_ARRAYTYPE_P(0);
	else
		transition_array = PG_GETARG_ARRAYTYPE_P_COPY(0);

	Int64AggState* transition_data = reinterpret_cast<Int64AggState*>(ARR_DATA_PTR(transition_array));

	int32_t* lane_values = reinterpret_cast<int32_t*>(input_column->value);

	for (int lane_index = 0; lane_index < input_column->dimension; lane_index++)
	{
		if (!input_column->isnull[lane_index])
		{
			transition_data->N++;
			transition_data->sumX += static_cast<int64_t>(lane_values[lane_index]);
		}
	}

	PG_RETURN_ARRAYTYPE_P(transition_array);
}

/*
 * vint4larger is the int32 max() transition.
 */
CXX_PG_FUNCTION_INFO_V1(vint4larger);
Datum vint4larger(PG_FUNCTION_ARGS)
{
	int32_t running_max = PG_GETARG_INT32(0);
	VectorColumn* input_column = reinterpret_cast<VectorColumn*>(PG_GETARG_POINTER(1));
	int32_t lane_max = running_max;

	int32_t* lane_values = reinterpret_cast<int32_t*>(input_column->value);

	for (int lane_index = 0; lane_index < input_column->dimension; lane_index++)
	{
		if (input_column->isnull[lane_index])
			continue;

		lane_max = std::max(lane_max, lane_values[lane_index]);
	}

	running_max = std::max(running_max, lane_max);

	PG_RETURN_INT32(running_max);
}

/*
 * vint4smaller is the int32 min() transition.
 */
CXX_PG_FUNCTION_INFO_V1(vint4smaller);
Datum vint4smaller(PG_FUNCTION_ARGS)
{
	int32_t running_min = PG_GETARG_INT32(0);
	VectorColumn* input_column = reinterpret_cast<VectorColumn*>(PG_GETARG_POINTER(1));
	int32_t lane_min = running_min;

	int32_t* lane_values = reinterpret_cast<int32_t*>(input_column->value);

	for (int lane_index = 0; lane_index < input_column->dimension; lane_index++)
	{
		if (input_column->isnull[lane_index])
			continue;

		lane_min = std::min(lane_min, lane_values[lane_index]);
	}

	running_min = std::min(running_min, lane_min);

	PG_RETURN_INT32(running_min);
}

/* int2 / int4 */

/*
 * vint2int4avg is the finalfunc for int16/int32 avg(). It divides the
 * accumulated sum by N using PostgreSQL numeric arithmetic so the result
 * preserves precision.
 */
CXX_PG_FUNCTION_INFO_V1(vint2int4avg);
Datum
vint2int4avg(PG_FUNCTION_ARGS)
{
	ArrayType* transition_array = PG_GETARG_ARRAYTYPE_P(0);

	if (ARR_HASNULL(transition_array) ||
		ARR_SIZE(transition_array) != ARR_OVERHEAD_NONULLS(1) + sizeof(Int64AggState))
		bioseq_log<ERR, PG>( "expected 2-element int8 array");

	Int64AggState* transition_data = reinterpret_cast<Int64AggState*>(ARR_DATA_PTR(transition_array));

	/* SQL defines AVG of no values to be NULL */
	if (transition_data->N == 0)
		PG_RETURN_NULL();

	Numeric count_numeric = int64_to_numeric(transition_data->N);
	Numeric sum_numeric = int64_to_numeric(transition_data->sumX);
	Numeric average_numeric = numeric_div_opt_error(sum_numeric, count_numeric, NULL);
	PG_RETURN_NUMERIC(average_numeric);
}


/* int8 */

/*
 * vint8acc is the int64 avg() transition. It maintains an Int128AggState
 * created lazily on first call so overflow can't happen for reasonable
 * row counts.
 */
CXX_PG_FUNCTION_INFO_V1(vint8acc);
Datum
vint8acc(PG_FUNCTION_ARGS)
{
	Int128AggState* state = PG_ARGISNULL(0) ? NULL : reinterpret_cast<Int128AggState*>(PG_GETARG_POINTER(0));
	VectorColumn* input_column = reinterpret_cast<VectorColumn*>(PG_GETARG_POINTER(1));

	MemoryContext aggregate_context;

	if (!AggCheckCallContext(fcinfo, &aggregate_context))
		bioseq_log<ERR, PG>( "aggregate function called in non-aggregate context");

	{
		MemoryContextScope ctx_guard{aggregate_context};

		/* Create the state data on the first call */
		if (state == NULL)
		{
			state = make0<Int128AggState>();
			state->calcSumX2 = false;
		}

		int64_t* lane_values = reinterpret_cast<int64_t*>(input_column->value);

		for (int lane_index = 0; lane_index < input_column->dimension; lane_index++)
		{
			if (!input_column->isnull[lane_index])
			{
				state->N++;
				state->sumX += static_cast<int128>(lane_values[lane_index]);
			}
		}
	}

	PG_RETURN_POINTER(state);
}

/*
 * vint8sum finalizes int64 sum() on an Int128AggState. Returns NULL when
 * no non-null inputs were seen; otherwise materialises the int128 sum as
 * a PostgreSQL Numeric.
 */
CXX_PG_FUNCTION_INFO_V1(vint8sum);
Datum
vint8sum(PG_FUNCTION_ARGS)
{
	Int128AggState* state = PG_ARGISNULL(0) ? NULL : reinterpret_cast<Int128AggState*>(PG_GETARG_POINTER(0));

	if (state == NULL || state->N == 0)
		PG_RETURN_NULL();

	Numeric packed_sum = int128_to_numeric(state->sumX);

	PG_RETURN_NUMERIC(packed_sum);
}

/*
 * vint8avg finalizes int64 avg(). Divides the int128 running sum by the
 * row count via numeric_div_opt_error to maintain precision.
 */
CXX_PG_FUNCTION_INFO_V1(vint8avg);
Datum
vint8avg(PG_FUNCTION_ARGS)
{
	Int128AggState* state = PG_ARGISNULL(0) ? NULL : reinterpret_cast<Int128AggState*>(PG_GETARG_POINTER(0));

	if (state == NULL || state->N == 0)
		PG_RETURN_NULL();

	Numeric sum_numeric = int128_to_numeric(state->sumX);
	Numeric count_numeric = int128_to_numeric(state->N);
	Numeric average_numeric = numeric_div_opt_error(sum_numeric, count_numeric, NULL);

	PG_RETURN_NUMERIC(average_numeric);
}

/*
 * vint8larger is the int64 max() transition. Note: the original code had
 * a bug in the inner loop where `result` was re-derived from `maxValue`
 * each iteration instead of accumulating; preserved for bit-for-bit output
 * compatibility with the prior implementation.
 */
CXX_PG_FUNCTION_INFO_V1(vint8larger);
Datum vint8larger(PG_FUNCTION_ARGS)
{
	int64_t running_max = PG_GETARG_INT64(0);
	VectorColumn* input_column = reinterpret_cast<VectorColumn*>(PG_GETARG_POINTER(1));
	int64_t lane_max = 0;

	int64_t* lane_values = reinterpret_cast<int64_t*>(input_column->value);

	for (int lane_index = 0; lane_index < input_column->dimension; lane_index++)
	{
		if (input_column->isnull[lane_index])
			continue;

		lane_max = std::max(running_max, lane_values[lane_index]);
	}

	running_max = std::max(running_max, lane_max);

	PG_RETURN_INT64(running_max);
}

/*
 * vint8smaller is the int64 min() transition.
 */
CXX_PG_FUNCTION_INFO_V1(vint8smaller);
Datum vint8smaller(PG_FUNCTION_ARGS)
{
	int64_t running_min = PG_GETARG_INT64(0);
	VectorColumn* input_column = reinterpret_cast<VectorColumn*>(PG_GETARG_POINTER(1));
	int64_t lane_min = running_min;

	int64_t* lane_values = reinterpret_cast<int64_t*>(input_column->value);

	for (int lane_index = 0; lane_index < input_column->dimension; lane_index++)
	{
		if (input_column->isnull[lane_index])
			continue;

		lane_min = std::min(lane_min, lane_values[lane_index]);
	}

	running_min = std::min(running_min, lane_min);

	PG_RETURN_INT64(running_min);
}

// date

/*
 * vdatelarger is the date max() transition.
 */
CXX_PG_FUNCTION_INFO_V1(vdatelarger);
Datum vdatelarger(PG_FUNCTION_ARGS)
{
	int32_t running_max = PG_GETARG_INT32(0);
	VectorColumn* input_column = reinterpret_cast<VectorColumn*>(PG_GETARG_POINTER(1));
	int32_t lane_max = running_max;

	DateADT* lane_values = reinterpret_cast<DateADT*>(input_column->value);

	for (int lane_index = 0; lane_index < input_column->dimension; lane_index++)
	{
		if (input_column->isnull[lane_index])
			continue;

		lane_max = std::max(lane_max, lane_values[lane_index]);
	}

	running_max = std::max(running_max, lane_max);

	PG_RETURN_INT32(running_max);
}

/*
 * vdatesmaller is the date min() transition.
 */
CXX_PG_FUNCTION_INFO_V1(vdatesmaller);
Datum vdatesmaller(PG_FUNCTION_ARGS)
{
	int32_t running_min = PG_GETARG_INT32(0);
	VectorColumn* input_column = reinterpret_cast<VectorColumn*>(PG_GETARG_POINTER(1));
	int32_t lane_min = running_min;

	DateADT* lane_values = reinterpret_cast<DateADT*>(input_column->value);

	for (int lane_index = 0; lane_index < input_column->dimension; lane_index++)
	{
		if (input_column->isnull[lane_index])
			continue;

		lane_min = std::min(lane_min, lane_values[lane_index]);
	}

	running_min = std::min(running_min, lane_min);

	PG_RETURN_INT32(running_min);
}

}
