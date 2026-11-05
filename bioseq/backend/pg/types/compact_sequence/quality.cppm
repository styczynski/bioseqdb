/** @file Thin SQL adapters for exact native-quality statistics. */
module;
#include <new>
extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/errcodes.h"
#include "utils/expandeddatum.h"
#include "access/htup_details.h"
#include "nodes/memnodes.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/typcache.h"
}
#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

export module bioseq.postgres.types.compact_sequence:quality;
import std;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.support;
import bioseq.postgres.support.sql_boundary;
import bioseq.sequences;
import bioseq.sequences.quality_summary;
import :flat_abi;
import :source_abi;
import :tree_types;
import :view_session;
import :packed_output;
import :letter;

export namespace bioseq::pg_types {
struct [[bioseq_sql::type({.schema = bioseq::sql::fixed_schema<"bioseq">})]] quality_stats_result {
    [[bioseq_sql::column({.sql_type = bioseq::sql::pg::bigint})]] std::int64_t length;
    [[bioseq_sql::column({.sql_type = bioseq::sql::pg::smallint})]] std::optional<std::int16_t> minimum;
    [[bioseq_sql::column({.sql_type = bioseq::sql::pg::smallint})]] std::optional<std::int16_t> maximum;
    [[bioseq_sql::column({.sql_type = bioseq::sql::pg::numeric})]] Numeric total;
    [[bioseq_sql::column({.sql_type = bioseq::sql::pg::numeric})]] std::optional<Numeric> mean;
};
}

export enum class pg_quality_statistic : std::uint8_t { none, minimum, maximum, total, mean, all };

/** Translate typed failures after the noPG operation has returned. */
export [[noreturn]] void pg_raise_quality_error(bioseq::sequences::quality_error error) {
    bioseq::postgres::support::invoke_pg_as_cpp([error] {
        if (error == bioseq::sequences::quality_error::unsupported_version)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("unsupported symbol_counts quality payload version"),
                errhint("Rebuild the symbol_counts index with this extension version.")));
        ereport(ERROR, (errcode(error == bioseq::sequences::quality_error::length_overflow
            ? ERRCODE_PROGRAM_LIMIT_EXCEEDED : error == bioseq::sequences::quality_error::memory_limit
                ? ERRCODE_OUT_OF_MEMORY : ERRCODE_DATA_CORRUPTED),
            errmsg("invalid quality summary or source (error %u)", static_cast<unsigned>(error))));
    });
    pg_unreachable();
}

export [[nodiscard]] bioseq::sequences::quality_summary pg_quality_summary(
        Datum input, pg_quality_read_statistics* statistics = nullptr) {
    const auto header = bioseq::postgres::support::invoke_pg_as_cpp(
        [input] { return classify_and_describe_sequence_header(input); });
    if (!header.alphabet_bound || !is_quality_alphabet_code(header.alphabet_code))
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH),
                errmsg("quality statistics require a bound quality sequence")));
        });
    if (header.kind == pg_sequence_datum_kind::locator)
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                errmsg("quality locator requires its owning table scan")));
        });
    using result_type = std::expected<bioseq::sequences::quality_summary, bioseq::sequences::quality_error>;
    auto result = with_pg_sequence_session<result_type>([input, header, statistics](pg_sequence_read_session& session) {
        result_type result = std::unexpected(bioseq::sequences::quality_error::invalid_summary);
        pgseq_supported_quality_alphabets::dispatch_alphabet_by_code(header.alphabet_code,
            [input, header, &session, &result]<class Alphabet>() {
                if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
                    result = summarize_pg_quality_tree<Alphabet>(session, get_pg_sequence_expanded_ro(input)->root);
                } else {
                    auto* flat = bioseq::postgres::support::invoke_pg_as_cpp([input, &session] {
                        const auto previous = MemoryContextSwitchTo(session.payload_context);
                        auto* value = PG_DETOAST_DATUM_COPY(input);
                        MemoryContextSwitchTo(previous);
                        return value;
                    });
                    auto* owner = make_pg_sequence_call_owner<Alphabet>(session, PointerGetDatum(flat),
                        classify_and_describe_sequence(PointerGetDatum(flat)));
                    const auto source = owner->view().template borrow_rank_source<false>();
                    result = bioseq::sequences::summarize_quality_source<Alphabet>(source, 0, source.symbol_count);
                    ++session.quality_statistics.summary_extractions;
                    ++session.quality_statistics.fallbacks;
                    session.quality_statistics.source_symbols += source.symbol_count;
                }
            });
        if (statistics) statistics->add(session.quality_statistics);
        return result;
    });
    if (!result) pg_raise_quality_error(result.error());
    return *result;
}

namespace {
#ifdef BIOSEQ_TEST_INDEXES
thread_local pg_quality_read_statistics scalar_quality_statistics{};
#endif

// Owned bytes identify immutable materialized values, including external TOAST
// identities. Never memoize an expanded source or an indirect borrowed pointer here:
// source-backed values use their snapshot/lifetime-checked provider resolver.
bioseq::sequences::quality_summary pg_quality_scalar_summary(
        Datum input) {
    const auto reduce = [input] {
#ifdef BIOSEQ_TEST_INDEXES
        return pg_quality_summary(input, &scalar_quality_statistics);
#else
        return pg_quality_summary(input);
#endif
    };
    const auto* bytes = reinterpret_cast<const std::byte*>(DatumGetPointer(input));
    if ((VARATT_IS_EXTERNAL(bytes) && !VARATT_IS_EXTERNAL_ONDISK(bytes))
        || CurrentMemoryContext == TopMemoryContext || CurrentMemoryContext == CacheMemoryContext)
        return reduce();
    const auto identity = std::span{bytes, static_cast<std::size_t>(VARSIZE_ANY(bytes))};
    if (const auto cached = find_pg_quality_summary(pg_quality_summary_identity::materialized, identity)) {
#ifdef BIOSEQ_TEST_INDEXES
        ++scalar_quality_statistics.cache_hits;
#endif
        return *cached;
    }
    const auto value = reduce();
    remember_pg_quality_summary(pg_quality_summary_identity::materialized, identity, value);
    return value;
}
} // namespace

/** Use PostgreSQL's public numeric input, after exact decimal construction in noPG. */
export [[nodiscard]] Datum pg_quality_numeric(const std::string& decimal) {
    return bioseq::postgres::support::invoke_pg_as_cpp([&decimal] {
        return DirectFunctionCall3(numeric_in, CStringGetDatum(decimal.c_str()),
            ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
    });
}

/** Row-local callers select outputs from one shared summary. */
export [[nodiscard]] std::optional<Datum> pg_quality_statistic_value(
        const bioseq::sequences::quality_summary& summary, pg_quality_statistic kind) {
    using namespace bioseq::sequences;
    if (kind == pg_quality_statistic::total) return pg_quality_numeric(quality_sum_decimal(summary.sum));
    if (kind == pg_quality_statistic::all) {
        std::array<Datum, 5> values{Int64GetDatum(summary.length), Int16GetDatum(summary.minimum),
            Int16GetDatum(summary.maximum), pg_quality_numeric(quality_sum_decimal(summary.sum)),
            summary.length ? pg_quality_numeric(*quality_mean_decimal(summary)) : Datum{0}};
        std::array<bool, 5> nulls{false, !summary.length, !summary.length, false, !summary.length};
        return bioseq::postgres::support::invoke_pg_as_cpp([&values, &nulls] {
            const auto oid = DatumGetObjectId(DirectFunctionCall1(regtypein,
                CStringGetDatum("bioseq.quality_stats_result")));
            auto* descriptor = lookup_rowtype_tupdesc(oid, -1);
            auto* tuple = heap_form_tuple(descriptor, values.data(), nulls.data());
            ReleaseTupleDesc(descriptor);
            return HeapTupleGetDatum(tuple);
        });
    }
    if (!summary.length) return std::nullopt;
    if (kind == pg_quality_statistic::minimum) return Int16GetDatum(summary.minimum);
    if (kind == pg_quality_statistic::maximum) return Int16GetDatum(summary.maximum);
    return pg_quality_numeric(*quality_mean_decimal(summary));
}

export namespace bioseq::pg_types::sql_routines {
/** Test-only counters for ordinary scalar calls (0 reset, 1 ranks, 2 reductions, 3 hits). */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .parallel = bioseq::sql::parallel_safety::restricted})]]
[[bioseq_sql::revoke({.privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role})]]
std::int64_t quality_scalar_summary_probe(std::int32_t action) {
#ifdef BIOSEQ_TEST_INDEXES
    std::uint64_t value{};
    switch (action) {
        case 0: scalar_quality_statistics = {}; return 0;
        case 1: value = scalar_quality_statistics.source_symbols; break;
        case 2: value = scalar_quality_statistics.summary_extractions; break;
        case 3: value = scalar_quality_statistics.cache_hits; break;
        default:
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("invalid quality scalar summary probe action")));
    }
    return static_cast<std::int64_t>(std::min(value,
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
#else
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        errmsg("quality scalar summary probe requires BIOSEQ_TEST_INDEXES")));
    pg_unreachable();
#endif
}

[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_symbol_count_support"})]]
std::optional<std::int16_t> min(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum input) {
    const auto value = pg_quality_scalar_summary(input);
    return value.length ? std::optional{value.minimum} : std::nullopt;
}
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_symbol_count_support"})]]
std::optional<std::int16_t> max(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum input) {
    const auto value = pg_quality_scalar_summary(input);
    return value.length ? std::optional{value.maximum} : std::nullopt;
}
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_symbol_count_support"})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::pg::numeric})]]
bioseq::postgres::support::numeric_result sum(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum input) {
    return bioseq::postgres::support::numeric_result::from_numeric(DatumGetNumeric(
        *pg_quality_statistic_value(pg_quality_scalar_summary(input), pg_quality_statistic::total)));
}
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_symbol_count_support"})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::pg::numeric})]]
std::optional<bioseq::postgres::support::numeric_result> avg(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum input) {
    const auto value = pg_quality_statistic_value(pg_quality_scalar_summary(input), pg_quality_statistic::mean);
    if (!value) return std::nullopt;
    return bioseq::postgres::support::numeric_result::from_numeric(DatumGetNumeric(*value));
}
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_symbol_count_support"})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.quality_stats_result">})]]
Datum quality_stats(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum input) {
    return *pg_quality_statistic_value(pg_quality_scalar_summary(input), pg_quality_statistic::all);
}
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
std::int16_t quality_score(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.letter">})]] bioseq::pg_types::letter input) {
    if (!is_quality_alphabet_code(input.alphabet_code))
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH),
                errmsg("quality_score requires a quality letter")));
        });
    std::int16_t score{};
    pgseq_supported_quality_alphabets::dispatch_alphabet_by_code(input.alphabet_code,
        [&score, input]<class Alphabet>() {
            if (input.rank >= seqan3::alphabet_size<Alphabet>)
                pg_raise_quality_error(bioseq::sequences::quality_error::invalid_rank);
            score = input.rank + bioseq::sequences::quality_alphabet_traits<Alphabet>::score_offset;
        });
    return score;
}
}
