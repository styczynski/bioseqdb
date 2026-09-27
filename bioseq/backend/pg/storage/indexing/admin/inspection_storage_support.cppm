/**
 * @file admin/inspection_storage_support.cppm
 * @brief Owns physical index-shard inspection helpers.
 */
module;

extern "C" {
#include "postgres.h"

#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/errcodes.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

export module bioseq.postgres.storage.indexing.admin:inspection_storage_support;

import bioseq.common.logging;
import bioseq.indexing.type;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.index_relation_identity;

export namespace bioseq::storage::indexing {

/// Raise one fail-closed physical index audit error.
[[noreturn]] inline void index_audit_error(std::string_view message) {
    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq physical minimizer-index audit: %.*s",
                    static_cast<int>(message.size()),
                    message.data())));
    pg_unreachable();
}

/// Open the SPI session required by one index audit query.
inline void spi_connect_for_index_audit() {
    if (SPI_connect() != SPI_OK_CONNECT) {
        index_audit_error("SPI connection failed");
    }
}

/// Execute an audit query that must return one nonnegative bigint.
[[nodiscard]] inline std::int64_t index_audit_scalar_i64(const std::string& sql) {
    spi_connect_for_index_audit();
    const int status = SPI_execute(sql.c_str(), true, 0);
    if (status != SPI_OK_SELECT || SPI_processed != 1 || SPI_tuptable == nullptr) {
        SPI_finish();
        index_audit_error("catalog query did not return exactly one row");
    }
    bool is_null = false;
    const Datum value = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &is_null);
    if (is_null) {
        SPI_finish();
        index_audit_error("catalog query returned NULL");
    }
    const std::int64_t result = DatumGetInt64(value);
    SPI_finish();
    if (result < 0) {
        index_audit_error("catalog query returned a negative count");
    }
    return result;
}

/// Derive every physical index-name prefix for a relation's sequence columns.
[[nodiscard]] inline std::vector<std::string> seqidx_prefixes_for(Oid rel_oid,
                                                                  std::string_view name) {
    const relation_identity rid = index_relation_identity(rel_oid);
    std::vector<std::string> prefixes;
    spi_connect_for_index_audit();
    const std::string sql =
        fmt::format("SELECT a.attnum FROM pg_attribute a JOIN pg_type t ON a.atttypid=t.oid JOIN "
                    "pg_namespace n ON t.typnamespace=n.oid "
                    "WHERE a.attrelid={}::oid AND t.typname='seq' AND n.nspname IN "
                    "('bioseq','bioseq_internal') AND a.attnum>0 AND NOT a.attisdropped",
                    static_cast<unsigned>(rel_oid));
    if (SPI_execute(sql.c_str(), true, 0) != SPI_OK_SELECT || SPI_tuptable == nullptr) {
        SPI_finish();
        index_audit_error("sequence-column catalog lookup failed");
    }
    for (std::uint64_t row = 0; row < SPI_processed; ++row) {
        bool is_null = false;
        const Datum value =
            SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1, &is_null);
        if (is_null) {
            SPI_finish();
            index_audit_error("sequence-column catalog lookup returned NULL");
        }
        const std::int16_t attno = DatumGetInt16(value);
        prefixes.push_back(fmt::format(
            "seqidx_{}_{:016x}_",
            short_index_for(name),
            identity_hash64(rid.db_oid, rid.tablespace_oid, rid.relfilenode, attno, name)));
    }
    SPI_finish();
    if (prefixes.empty()) {
        index_audit_error("target has no visible bioseq.seq column");
    }
    return prefixes;
}

/// List physical index shard relations matching one escaped prefix.
[[nodiscard]] inline std::vector<std::string>
shard_relnames_with_prefix(const std::string& prefix) {
    std::vector<std::string> out;
    spi_connect_for_index_audit();
    std::string escaped;
    for (const char character : prefix) {
        if (character == '_' || character == '%' || character == '\\') {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    const std::string sql = fmt::format(
        "SELECT c.relname FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname='bioseq_internal' AND c.relkind='r' AND c.relname LIKE '{}%' ESCAPE '\\'",
        escaped);
    if (SPI_execute(sql.c_str(), true, 0) != SPI_OK_SELECT || SPI_tuptable == nullptr) {
        SPI_finish();
        index_audit_error("physical index relation lookup failed");
    }
    for (std::uint64_t row = 0; row < SPI_processed; ++row) {
        char* name = SPI_getvalue(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1);
        if (name == nullptr) {
            SPI_finish();
            index_audit_error("physical index relation lookup returned NULL");
        }
        out.emplace_back(name);
    }
    SPI_finish();
    return out;
}

/// Count rows in one quoted physical index shard with an optional predicate.
[[nodiscard]] inline std::int64_t count_in_shard(const std::string& relname,
                                                 const std::string& where_clause) {
    return index_audit_scalar_i64(fmt::format("SELECT count(*)::bigint FROM bioseq_internal.{} {}",
                                              quote_identifier(relname.c_str()),
                                              where_clause));
}

/// Check whether a shard has the complete grouped-generation key schema.
[[nodiscard]] inline bool shard_has_minimizer_generation_key(const std::string& relname) {
    const std::int64_t matching_attributes = index_audit_scalar_i64(
        fmt::format("SELECT count(*)::bigint FROM pg_attribute "
                    "WHERE attrelid='bioseq_internal.{}'::regclass "
                    "AND attnum>0 AND NOT attisdropped AND ("
                    "(attname='generation_id' AND atttypid='int8'::regtype) OR "
                    "(attname='position_segment' AND atttypid='int4'::regtype) OR "
                    "(attname='sub_key' AND atttypid='int8'::regtype) OR "
                    "(attname='chunk_no' AND atttypid='int4'::regtype))",
                    quote_identifier(relname.c_str())));
    return matching_attributes == 4;
}

/// Return the largest per-shard distinct published sub-key count.
[[nodiscard]] inline std::int64_t
index_distinct_sub_keys_impl(Oid rel_oid, std::string_view name, std::int64_t query_key) {
    std::int64_t best = 0;
    const std::string where = fmt::format("WHERE query_key={} AND sub_key < {}",
                                          (long long)query_key,
                                          (long long)k_temporary_run_sub_key_base);
    for (const std::string& pfx : seqidx_prefixes_for(rel_oid, name))
        for (const std::string& sh : shard_relnames_with_prefix(pfx)) {
            // distinct sub_key in this shard for this query_key
            if (SPI_connect() != SPI_OK_CONNECT)
                continue;
            std::string sql =
                fmt::format("SELECT count(DISTINCT sub_key)::bigint FROM bioseq_internal.{} {}",
                            quote_identifier(sh.c_str()),
                            where);
            if (SPI_execute(sql.c_str(), true, 0) == SPI_OK_SELECT && SPI_processed > 0) {
                bool isn = false;
                std::int64_t v = DatumGetInt64(
                    SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isn));
                if (!isn && v > best)
                    best = v;
            }
            SPI_finish();
        }
    return best;
}

} // namespace bioseq::storage::indexing
