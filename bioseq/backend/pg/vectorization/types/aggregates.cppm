/**
 * @file
 * Define SQL aggregates that update their state from a vector batch.
 */


module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "nodes/execnodes.h"
#include "utils/fmgroids.h"
}

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

export module bioseq.postgres.vectorization.types.aggregates;

import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.support;
import bioseq.postgres.support.sql_boundary;
import bioseq.postgres.vectorization.types;
import bioseq.postgres.vectorization.types.numeric;

namespace {

#if defined(BIOSEQ_TEST_INDEXES)
/** Aggregate routines whose calls are counted by the runtime test. */
enum class aggregate_owner_runtime_id : std::size_t {
    vemptycount,
    vanycount,
    vint2sum,
    vint2acc,
    vint2larger,
    vint2smaller,
    vint4sum,
    vint4acc,
    vint4larger,
    vint4smaller,
    vint2int4avg,
    vint8acc,
    vint8sum,
    vint8avg,
    vint8larger,
    vint8smaller,
    vdatelarger,
    vdatesmaller,
    count,
};

/** Calls to each aggregate routine on the current backend thread. */
inline thread_local std::array<std::uint64_t,
                               static_cast<std::size_t>(aggregate_owner_runtime_id::count)>
    aggregate_owner_runtime_visits{};

/** Count one aggregate routine call for the runtime test. */
inline void note_aggregate_owner_visit(aggregate_owner_runtime_id owner) noexcept {
    ++aggregate_owner_runtime_visits[static_cast<std::size_t>(owner)];
}
#else
enum class aggregate_owner_runtime_id : std::size_t {};
inline void note_aggregate_owner_visit(aggregate_owner_runtime_id) noexcept {}
#endif

template <class p_state, class p_column, class p_fold>
/** Update an existing state with each non-NULL vector value.
 * A NULL state or missing input leaves the state unchanged. */
[[nodiscard]] std::optional<p_state>
fold_vector(std::optional<p_state> state, const std::optional<p_column>& input, p_fold&& fold) {
    if (!state.has_value() || !input.has_value()) {
        return state;
    }
    for (std::size_t lane_index = 0; lane_index < input->size(); ++lane_index) {
        if (!input->is_null(lane_index)) {
            std::invoke(fold, *state, input->value(lane_index));
        }
    }
    return state;
}

template <auto p_sql_type, class p_value_type>
/** Add non-NULL SMALLINT or INTEGER values to the count and sum used by AVG. */
[[nodiscard]] std::optional<bioseq::postgres::support::int64_array_aggregate_state>
accumulate_integer_average_state(
    std::optional<bioseq::postgres::support::int64_array_aggregate_state> state,
    const std::optional<bioseq::postgres::support::vector_column<p_sql_type, p_value_type>>&
        input) {
    if (!state.has_value() || !input.has_value()) {
        return state;
    }
    int64_aggregate_state_storage& accumulator = state->value();
    for (std::size_t lane_index = 0; lane_index < input->size(); ++lane_index) {
        if (!input->is_null(lane_index)) {
            ++accumulator.count;
            accumulator.sum += static_cast<std::int64_t>(input->value(lane_index));
        }
    }
    return state;
}

/** Return sum divided by count using PostgreSQL numeric arithmetic. */
[[nodiscard]] bioseq::postgres::support::numeric_result numeric_average(std::int64_t count,
                                                                        std::int64_t sum) {
    Numeric count_numeric =
        bioseq::postgres::support::invoke_pg_as_cpp([count] { return int64_to_numeric(count); });
    Numeric sum_numeric =
        bioseq::postgres::support::invoke_pg_as_cpp([sum] { return int64_to_numeric(sum); });
    Numeric quotient_numeric =
        bioseq::postgres::support::invoke_pg_as_cpp([sum_numeric, count_numeric] {
#if PG_VERSION_NUM >= 190000
            return numeric_div_safe(sum_numeric, count_numeric, nullptr);
#else
            return numeric_div_opt_error(sum_numeric, count_numeric, nullptr);
#endif
        });
    return bioseq::postgres::support::numeric_result::from_numeric(quotient_numeric);
}

/** Convert the 128-bit sum to numeric and divide it by the stored count. */
[[nodiscard]] bioseq::postgres::support::numeric_result
numeric_average(const int128_aggregate_state_storage& state) {
    Numeric sum_numeric = bioseq::postgres::support::invoke_pg_as_cpp(
        [sum = state.sum] { return int128_to_numeric(sum); });
    Numeric count_numeric = bioseq::postgres::support::invoke_pg_as_cpp(
        [count = state.count] { return int128_to_numeric(count); });
    Numeric quotient_numeric =
        bioseq::postgres::support::invoke_pg_as_cpp([sum_numeric, count_numeric] {
#if PG_VERSION_NUM >= 190000
            return numeric_div_safe(sum_numeric, count_numeric, nullptr);
#else
            return numeric_div_opt_error(sum_numeric, count_numeric, nullptr);
#endif
        });
    return bioseq::postgres::support::numeric_result::from_numeric(quotient_numeric);
}

} // namespace

export namespace bioseq::vector::aggregate_sql_routines {

/** Keep the COUNT(*) state unchanged here. The vector executor adds the batch's row count. */
[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]] std::optional<std::int64_t> vemptycount(std::optional<std::int64_t> running_count) {
#if defined(BIOSEQ_TEST_INDEXES)
    note_aggregate_owner_visit(aggregate_owner_runtime_id::vemptycount);
#endif
    return running_count;
}

/** Add the batch's non-NULL row count to an initialized COUNT state. */
[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]] std::optional<std::int64_t>
    vanycount(std::optional<std::int64_t> running_count,
              [[bioseq_sql::param({
                  .sql_type = bioseq::sql::pg::quoted_any,
              })]] std::optional<bioseq::postgres::support::vector_null_mask> input) {
#if defined(BIOSEQ_TEST_INDEXES)
    note_aggregate_owner_visit(aggregate_owner_runtime_id::vanycount);
#endif
    if (!running_count.has_value() || !input.has_value()) {
        return running_count;
    }
    *running_count += static_cast<std::int64_t>(input->non_null_count());
    return running_count;
}

/** Add non-NULL SMALLINT values to an initialized bigint sum. */
[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]] std::optional<std::int64_t>
    vint2sum(std::optional<std::int64_t> running_sum,
             [[bioseq_sql::param({
                 .sql_type = bioseq::sql::pg::smallint,
             })]] std::optional<bioseq::postgres::support::vector_column<bioseq::sql::pg::smallint,
                                                                         std::int16_t>> input) {
#if defined(BIOSEQ_TEST_INDEXES)
    note_aggregate_owner_visit(aggregate_owner_runtime_id::vint2sum);
#endif
    return fold_vector(std::move(running_sum), input, [](std::int64_t& sum, std::int16_t value) {
        sum += static_cast<std::int64_t>(value);
    });
}

/** Add non-NULL SMALLINT values to the count and sum used by AVG. */
[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]] [[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::internal,
})]] std::optional<bioseq::postgres::support::int64_array_aggregate_state>
    vint2acc([[bioseq_sql::param({
                 .sql_type = bioseq::sql::pg::internal,
             })]] std::optional<bioseq::postgres::support::int64_array_aggregate_state> state,
             [[bioseq_sql::param({
                 .sql_type = bioseq::sql::pg::smallint,
             })]] std::optional<bioseq::postgres::support::vector_column<bioseq::sql::pg::smallint,
                                                                         std::int16_t>> input) {
#if defined(BIOSEQ_TEST_INDEXES)
    note_aggregate_owner_visit(aggregate_owner_runtime_id::vint2acc);
#endif
    return accumulate_integer_average_state(std::move(state), input);
}

/** Update an initialized SMALLINT maximum with the batch's non-NULL values. */
[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]] std::optional<std::int16_t>
    vint2larger(
        std::optional<std::int16_t> running_max,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::smallint,
        })]] std::optional<bioseq::postgres::support::vector_column<bioseq::sql::pg::smallint,
                                                                    std::int16_t>> input) {
#if defined(BIOSEQ_TEST_INDEXES)
    note_aggregate_owner_visit(aggregate_owner_runtime_id::vint2larger);
#endif
    return fold_vector(
        std::move(running_max), input, [](std::int16_t& maximum, std::int16_t value) {
            maximum = std::max(maximum, value);
        });
}

/** Update an initialized SMALLINT minimum with the batch's non-NULL values. */
[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]] std::optional<std::int16_t>
    vint2smaller(
        std::optional<std::int16_t> running_min,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::smallint,
        })]] std::optional<bioseq::postgres::support::vector_column<bioseq::sql::pg::smallint,
                                                                    std::int16_t>> input) {
#if defined(BIOSEQ_TEST_INDEXES)
    note_aggregate_owner_visit(aggregate_owner_runtime_id::vint2smaller);
#endif
    return fold_vector(
        std::move(running_min), input, [](std::int16_t& minimum, std::int16_t value) {
            minimum = std::min(minimum, value);
        });
}

/** Add non-NULL INTEGER values to an initialized bigint sum. */
[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]] std::optional<std::int64_t>
    vint4sum(std::optional<std::int64_t> running_sum,
             [[bioseq_sql::param({
                 .sql_type = bioseq::sql::pg::integer,
             })]] std::optional<bioseq::postgres::support::vector_column<bioseq::sql::pg::integer,
                                                                         std::int32_t>> input) {
#if defined(BIOSEQ_TEST_INDEXES)
    note_aggregate_owner_visit(aggregate_owner_runtime_id::vint4sum);
#endif
    return fold_vector(std::move(running_sum), input, [](std::int64_t& sum, std::int32_t value) {
        sum += static_cast<std::int64_t>(value);
    });
}

/** Add non-NULL INTEGER values to the count and sum used by AVG. */
[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]] [[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::internal,
})]] std::optional<bioseq::postgres::support::int64_array_aggregate_state>
    vint4acc([[bioseq_sql::param({
                 .sql_type = bioseq::sql::pg::internal,
             })]] std::optional<bioseq::postgres::support::int64_array_aggregate_state> state,
             [[bioseq_sql::param({
                 .sql_type = bioseq::sql::pg::integer,
             })]] std::optional<bioseq::postgres::support::vector_column<bioseq::sql::pg::integer,
                                                                         std::int32_t>> input) {
#if defined(BIOSEQ_TEST_INDEXES)
    note_aggregate_owner_visit(aggregate_owner_runtime_id::vint4acc);
#endif
    return accumulate_integer_average_state(std::move(state), input);
}

/** Update an initialized INTEGER maximum with the batch's non-NULL values. */
[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]] std::optional<std::int32_t>
    vint4larger(
        std::optional<std::int32_t> running_max,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::integer,
        })]] std::optional<bioseq::postgres::support::vector_column<bioseq::sql::pg::integer,
                                                                    std::int32_t>> input) {
#if defined(BIOSEQ_TEST_INDEXES)
    note_aggregate_owner_visit(aggregate_owner_runtime_id::vint4larger);
#endif
    return fold_vector(
        std::move(running_max), input, [](std::int32_t& maximum, std::int32_t value) {
            maximum = std::max(maximum, value);
        });
}

/** Update an initialized INTEGER minimum with the batch's non-NULL values. */
[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]] std::optional<std::int32_t>
    vint4smaller(
        std::optional<std::int32_t> running_min,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::integer,
        })]] std::optional<bioseq::postgres::support::vector_column<bioseq::sql::pg::integer,
                                                                    std::int32_t>> input) {
#if defined(BIOSEQ_TEST_INDEXES)
    note_aggregate_owner_visit(aggregate_owner_runtime_id::vint4smaller);
#endif
    return fold_vector(
        std::move(running_min), input, [](std::int32_t& minimum, std::int32_t value) {
            minimum = std::min(minimum, value);
        });
}

/** Return a SMALLINT or INTEGER average as numeric, or NULL when no values were counted. */
[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]] [[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::numeric,
})]] std::optional<bioseq::postgres::support::numeric_result> vint2int4avg([[bioseq_sql::param({
    .sql_type = bioseq::sql::pg::internal,
})]] std::optional<bioseq::postgres::support::int64_array_aggregate_state> state) {
#if defined(BIOSEQ_TEST_INDEXES)
    note_aggregate_owner_visit(aggregate_owner_runtime_id::vint2int4avg);
#endif
    if (!state.has_value() || state->value().count == 0) {
        return std::nullopt;
    }
    return numeric_average(state->value().count, state->value().sum);
}

/** Accumulate BIGINT values in a 128-bit sum and count them for SUM or AVG.
 * Allocate missing state in the aggregate memory context and skip NULL rows. */
[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]] [[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::internal,
})]] std::optional<bioseq::postgres::support::int128_aggregate_state>
    vint8acc([[bioseq_sql::param({
                 .sql_type = bioseq::sql::pg::internal,
             })]] std::optional<bioseq::postgres::support::int128_aggregate_state> state,
             [[bioseq_sql::param({
                 .sql_type = bioseq::sql::pg::bigint,
             })]] std::optional<bioseq::postgres::support::vector_column<bioseq::sql::pg::bigint,
                                                                         std::int64_t>> input,
             bioseq::postgres::support::function_call_context context) {
#if defined(BIOSEQ_TEST_INDEXES)
    note_aggregate_owner_visit(aggregate_owner_runtime_id::vint8acc);
#endif
    if (!input.has_value()) {
        return state;
    }
    if (!state.has_value()) {
        const MemoryContext aggregate_context = context.aggregate_memory_context();
        int128_aggregate_state_storage* const allocated_state =
            bioseq::postgres::support::invoke_pg_as_cpp([aggregate_context] {
                return static_cast<int128_aggregate_state_storage*>(MemoryContextAllocZero(
                    aggregate_context, sizeof(int128_aggregate_state_storage)));
            });
        allocated_state->calculate_sum_of_squares = false;
        state = bioseq::postgres::support::int128_aggregate_state::from_new(allocated_state);
    }
    int128_aggregate_state_storage& accumulator = state->value();
    for (std::size_t lane_index = 0; lane_index < input->size(); ++lane_index) {
        if (!input->is_null(lane_index)) {
            ++accumulator.count;
            accumulator.sum += static_cast<int128>(input->value(lane_index));
        }
    }
    return state;
}

/** Return the BIGINT sum as numeric, or NULL when no values were counted. */
[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]] [[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::numeric,
})]] std::optional<bioseq::postgres::support::numeric_result> vint8sum([[bioseq_sql::param({
    .sql_type = bioseq::sql::pg::internal,
})]] std::optional<bioseq::postgres::support::int128_aggregate_state> state) {
#if defined(BIOSEQ_TEST_INDEXES)
    note_aggregate_owner_visit(aggregate_owner_runtime_id::vint8sum);
#endif
    if (!state.has_value() || state->value().count == 0) {
        return std::nullopt;
    }
    Numeric sum_numeric = bioseq::postgres::support::invoke_pg_as_cpp(
        [sum = state->value().sum] { return int128_to_numeric(sum); });
    return bioseq::postgres::support::numeric_result::from_numeric(sum_numeric);
}

/** Return the BIGINT average as numeric, or NULL when no values were counted. */
[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]] [[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::numeric,
})]] std::optional<bioseq::postgres::support::numeric_result> vint8avg([[bioseq_sql::param({
    .sql_type = bioseq::sql::pg::internal,
})]] std::optional<bioseq::postgres::support::int128_aggregate_state> state) {
#if defined(BIOSEQ_TEST_INDEXES)
    note_aggregate_owner_visit(aggregate_owner_runtime_id::vint8avg);
#endif
    if (!state.has_value() || state->value().count == 0) {
        return std::nullopt;
    }
    return numeric_average(state->value());
}

/** Update an initialized BIGINT maximum with the batch's non-NULL values. */
[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]] std::optional<std::int64_t>
    vint8larger(std::optional<std::int64_t> running_max,
                [[bioseq_sql::param({
                    .sql_type = bioseq::sql::pg::bigint,
                })]] std::optional<bioseq::postgres::support::vector_column<bioseq::sql::pg::bigint,
                                                                            std::int64_t>> input) {
#if defined(BIOSEQ_TEST_INDEXES)
    note_aggregate_owner_visit(aggregate_owner_runtime_id::vint8larger);
#endif
    return fold_vector(
        std::move(running_max), input, [](std::int64_t& maximum, std::int64_t value) {
            maximum = std::max(maximum, value);
        });
}

/** Update an initialized BIGINT minimum with the batch's non-NULL values. */
[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]] std::optional<std::int64_t>
    vint8smaller(
        std::optional<std::int64_t> running_min,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::bigint,
        })]] std::optional<bioseq::postgres::support::vector_column<bioseq::sql::pg::bigint,
                                                                    std::int64_t>> input) {
#if defined(BIOSEQ_TEST_INDEXES)
    note_aggregate_owner_visit(aggregate_owner_runtime_id::vint8smaller);
#endif
    return fold_vector(
        std::move(running_min), input, [](std::int64_t& minimum, std::int64_t value) {
            minimum = std::min(minimum, value);
        });
}

/** Update an initialized latest date with the batch's non-NULL dates. */
[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]] [[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::date,
})]] std::optional<DateADT>
    vdatelarger(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::date,
        })]] std::optional<DateADT> running_max,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::date,
        })]] std::optional<bioseq::postgres::support::vector_column<bioseq::sql::pg::date, DateADT>>
            input) {
#if defined(BIOSEQ_TEST_INDEXES)
    note_aggregate_owner_visit(aggregate_owner_runtime_id::vdatelarger);
#endif
    return fold_vector(std::move(running_max), input, [](DateADT& maximum, DateADT value) {
        maximum = std::max(maximum, value);
    });
}

/** Update an initialized earliest date with the batch's non-NULL dates. */
[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]] [[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::date,
})]] std::optional<DateADT>
    vdatesmaller(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::date,
        })]] std::optional<DateADT> running_min,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::date,
        })]] std::optional<bioseq::postgres::support::vector_column<bioseq::sql::pg::date, DateADT>>
            input) {
#if defined(BIOSEQ_TEST_INDEXES)
    note_aggregate_owner_visit(aggregate_owner_runtime_id::vdatesmaller);
#endif
    return fold_vector(std::move(running_min), input, [](DateADT& minimum, DateADT value) {
        minimum = std::min(minimum, value);
    });
}

#if defined(BIOSEQ_TEST_INDEXES)
/** Reset this backend thread's aggregate routine call counts. */
void reset_runtime_owner_visits() noexcept {
    aggregate_owner_runtime_visits.fill(0);
}

/** Fail the runtime test if any registered aggregate routine was never called. */
void validate_runtime_owner_visits() {
    static constexpr std::array<std::string_view, 18> owner_names{
        "vemptycount",
        "vanycount",
        "vint2sum",
        "vint2acc",
        "vint2larger",
        "vint2smaller",
        "vint4sum",
        "vint4acc",
        "vint4larger",
        "vint4smaller",
        "vint2int4avg",
        "vint8acc",
        "vint8sum",
        "vint8avg",
        "vint8larger",
        "vint8smaller",
        "vdatelarger",
        "vdatesmaller",
    };
    static_assert(owner_names.size() == aggregate_owner_runtime_visits.size());
    std::string missing_owner_names;
    for (std::size_t index = 0; index != owner_names.size(); ++index) {
        if (aggregate_owner_runtime_visits[index] != 0)
            continue;
        if (!missing_owner_names.empty())
            missing_owner_names.append(", ");
        missing_owner_names.append(owner_names[index]);
    }
    if (!missing_owner_names.empty()) {
        throw std::runtime_error{"aggregate owners were not executed: " + missing_owner_names};
    }
}

/** Check that PostgreSQL's int8 aggregate serializer preserves a sum wider than 64 bits. */
void validate_int8_serial_roundtrip() {
    ExprContext expression_context{};
    NodeSetTag(&expression_context, T_ExprContext);
    expression_context.ecxt_per_tuple_memory = CurrentMemoryContext;
    AggState aggregate_context{};
    NodeSetTag(&aggregate_context, T_AggState);
    aggregate_context.curaggcontext = &expression_context;

    int128_aggregate_state_storage source{};
    source.calculate_sum_of_squares = false;
    source.count = 3;
    source.sum = (static_cast<int128>(1) << 80) + static_cast<int128>(388);

    const Datum serialized_state_datum =
        bioseq::postgres::support::invoke_pg_as_cpp([&aggregate_context, &source] {
            FmgrInfo serialization_function_info{};
            fmgr_info(F_INT8_AVG_SERIALIZE, &serialization_function_info);
            LOCAL_FCINFO(serialization_call, 1);
            InitFunctionCallInfoData(*serialization_call,
                                     &serialization_function_info,
                                     1,
                                     InvalidOid,
                                     reinterpret_cast<Node*>(&aggregate_context),
                                     nullptr);
            serialization_call->args[0].value = PointerGetDatum(&source);
            serialization_call->args[0].isnull = false;
            const Datum serialized_result = FunctionCallInvoke(serialization_call);
            if (serialization_call->isnull) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_EXCEPTION),
                         errmsg_internal("int8_avg_serialize returned "
                                         "NULL")));
            }
            return serialized_result;
        });
    int128_aggregate_state_storage* const decoded_state =
        bioseq::postgres::support::invoke_pg_as_cpp([&aggregate_context, serialized_state_datum] {
            FmgrInfo deserialization_function_info{};
            fmgr_info(F_INT8_AVG_DESERIALIZE, &deserialization_function_info);
            LOCAL_FCINFO(deserialization_call, 2);
            InitFunctionCallInfoData(*deserialization_call,
                                     &deserialization_function_info,
                                     2,
                                     InvalidOid,
                                     reinterpret_cast<Node*>(&aggregate_context),
                                     nullptr);
            deserialization_call->args[0].value = serialized_state_datum;
            deserialization_call->args[0].isnull = false;
            deserialization_call->args[1].value = PointerGetDatum(nullptr);
            deserialization_call->args[1].isnull = true;
            const Datum deserialized_result = FunctionCallInvoke(deserialization_call);
            if (deserialization_call->isnull) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_EXCEPTION),
                         errmsg_internal("int8_avg_deserialize returned "
                                         "NULL")));
            }
            return reinterpret_cast<int128_aggregate_state_storage*>(
                DatumGetPointer(deserialized_result));
        });

    const bool roundtrip_matches =
        decoded_state != nullptr &&
        decoded_state->calculate_sum_of_squares == source.calculate_sum_of_squares &&
        decoded_state->count == source.count && decoded_state->sum == source.sum;
    bioseq::postgres::support::invoke_pg_as_cpp([serialized_state_datum, decoded_state] {
        if (decoded_state != nullptr)
            pfree(decoded_state);
        pfree(DatumGetPointer(serialized_state_datum));
    });
    if (!roundtrip_matches) {
        throw std::runtime_error{"int8 aggregate serialization state changed"};
    }
}
#endif

} // namespace bioseq::vector::aggregate_sql_routines

namespace bioseq::vector::aggregate_sql_owners {

namespace count_all {
[[bioseq_sql::aggregate({
    .transition = bioseq::vector::aggregate_sql_routines::vemptycount,
    .initial_condition = std::int64_t{0},
})]]
/** Register vector COUNT(*), with row counting performed by the executor. */
inline constexpr bioseq::sql::aggregate_tag vcount{};
}

namespace count_any {
[[bioseq_sql::aggregate({
    .transition = bioseq::vector::aggregate_sql_routines::vanycount,
    .initial_condition = std::int64_t{0},
})]]
/** Register vector COUNT(value), counting only non-NULL rows. */
inline constexpr bioseq::sql::aggregate_tag vcount{};
}

namespace smallint_sum {
[[bioseq_sql::aggregate({
    .transition = bioseq::vector::aggregate_sql_routines::vint2sum,
    .initial_condition = std::int64_t{0},
})]]
/** Register vector SMALLINT SUM with a bigint state initialized to zero. */
inline constexpr bioseq::sql::aggregate_tag vsum{};
}

namespace smallint_average {
[[bioseq_sql::aggregate({
    .transition = bioseq::vector::aggregate_sql_routines::vint2acc,
    .final = bioseq::vector::aggregate_sql_routines::vint2int4avg,
    .initial_condition = bioseq::sql::pg_input_text{"{0,0}"},
})]]
/** Register vector SMALLINT AVG using a count and sum, then numeric division. */
inline constexpr bioseq::sql::aggregate_tag vavg{};
}

namespace smallint_maximum {
[[bioseq_sql::aggregate({
    .transition = bioseq::vector::aggregate_sql_routines::vint2larger,
    .initial_condition = std::int64_t{-32768},
})]]
/** Register vector SMALLINT MAX with the smallest representable initial value. */
inline constexpr bioseq::sql::aggregate_tag vmax{};
}

namespace smallint_minimum {
[[bioseq_sql::aggregate({
    .transition = bioseq::vector::aggregate_sql_routines::vint2smaller,
    .initial_condition = std::int64_t{32767},
})]]
/** Register vector SMALLINT MIN with the largest representable initial value. */
inline constexpr bioseq::sql::aggregate_tag vmin{};
}

namespace integer_sum {
[[bioseq_sql::aggregate({
    .transition = bioseq::vector::aggregate_sql_routines::vint4sum,
    .initial_condition = std::int64_t{0},
})]]
/** Register vector INTEGER SUM with a bigint state initialized to zero. */
inline constexpr bioseq::sql::aggregate_tag vsum{};
}

namespace integer_average {
[[bioseq_sql::aggregate({
    .transition = bioseq::vector::aggregate_sql_routines::vint4acc,
    .final = bioseq::vector::aggregate_sql_routines::vint2int4avg,
    .initial_condition = bioseq::sql::pg_input_text{"{0,0}"},
})]]
/** Register vector INTEGER AVG using a count and sum, then numeric division. */
inline constexpr bioseq::sql::aggregate_tag vavg{};
}

namespace integer_maximum {
[[bioseq_sql::aggregate({
    .transition = bioseq::vector::aggregate_sql_routines::vint4larger,
    .initial_condition = std::int64_t{-2147483648},
})]]
/** Register vector INTEGER MAX with the smallest representable initial value. */
inline constexpr bioseq::sql::aggregate_tag vmax{};
}

namespace integer_minimum {
[[bioseq_sql::aggregate({
    .transition = bioseq::vector::aggregate_sql_routines::vint4smaller,
    .initial_condition = std::int64_t{2147483647},
})]]
/** Register vector INTEGER MIN with the largest representable initial value. */
inline constexpr bioseq::sql::aggregate_tag vmin{};
}

namespace bigint_sum {
[[bioseq_sql::aggregate({
    .transition = bioseq::vector::aggregate_sql_routines::vint8acc,
    .final = bioseq::vector::aggregate_sql_routines::vint8sum,
    .serial = bioseq::sql::pg_catalog_reference::int8_avg_serialize,
    .deserial = bioseq::sql::pg_catalog_reference::int8_avg_deserialize,
})]]
/** Register vector BIGINT SUM with 128-bit accumulation and PostgreSQL state serialization. */
inline constexpr bioseq::sql::aggregate_tag vsum{};
}

namespace bigint_average {
[[bioseq_sql::aggregate({
    .transition = bioseq::vector::aggregate_sql_routines::vint8acc,
    .final = bioseq::vector::aggregate_sql_routines::vint8avg,
    .serial = bioseq::sql::pg_catalog_reference::int8_avg_serialize,
    .deserial = bioseq::sql::pg_catalog_reference::int8_avg_deserialize,
})]]
/** Register vector BIGINT AVG with 128-bit accumulation and PostgreSQL state serialization. */
inline constexpr bioseq::sql::aggregate_tag vavg{};
}

namespace bigint_maximum {
[[bioseq_sql::aggregate({
    .transition = bioseq::vector::aggregate_sql_routines::vint8larger,
    .initial_condition = std::int64_t{-9223372036854775807LL - 1},
})]]
/** Register vector BIGINT MAX with the smallest representable initial value. */
inline constexpr bioseq::sql::aggregate_tag vmax{};
}

namespace bigint_minimum {
[[bioseq_sql::aggregate({
    .transition = bioseq::vector::aggregate_sql_routines::vint8smaller,
    .initial_condition = std::int64_t{9223372036854775807LL},
})]]
/** Register vector BIGINT MIN with the largest representable initial value. */
inline constexpr bioseq::sql::aggregate_tag vmin{};
}

namespace date_maximum {
[[bioseq_sql::aggregate({
    .transition = bioseq::vector::aggregate_sql_routines::vdatelarger,
    .initial_condition = bioseq::sql::pg_infinity::negative,
})]]
/** Register vector DATE MAX starting from negative infinity. */
inline constexpr bioseq::sql::aggregate_tag vmax{};
}

namespace date_minimum {
[[bioseq_sql::aggregate({
    .transition = bioseq::vector::aggregate_sql_routines::vdatesmaller,
    .initial_condition = bioseq::sql::pg_infinity::positive,
})]]
/** Register vector DATE MIN starting from positive infinity. */
inline constexpr bioseq::sql::aggregate_tag vmin{};
}

} // namespace bioseq::vector::aggregate_sql_owners
