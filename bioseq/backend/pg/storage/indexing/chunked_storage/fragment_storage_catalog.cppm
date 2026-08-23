/**
 * @file fragment_storage_catalog.cppm
 * @brief Fragment storage relation creation, discovery, and generation session entry points.
 */
module;
#include <bioseq/pg_compat.h>
extern "C" {
#include "access/heapam.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/namespace.h" // RelnameGetRelid
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type_d.h"
#include "commands/tablespace.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "miscadmin.h"
}

// Clang 22 must see these textual libc++ declarations before deserializing
// header-backed dependency BMIs; importing std first makes aligned operator new
// appear as two unrelated candidates during container allocation.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.postgres.storage.indexing.chunked_storage:fragment_storage_catalog;
import bioseq.postgres.support; // bioseq::postgres::support::try_open_postgres_relation, bioseq::postgres::support::find_primary_key_oid
import :catalog;
import :fragment_image;
import :generation_writer;
import :generation_session;
import :hash_major_writer;
export import :fragment_storage_contract;
export namespace bioseq::storage::indexing {

/// Parsed bucket and shard from one canonical generation sidecar name.
struct canonical_generation_sidecar_name {
    bool valid = false;
    std::uint8_t bucket_idx = 0;
    std::uint32_t shard_idx = 0;
};

/// Parse only the exact suffix spelling emitted by `format_seqidx_name`.
/// Leading zeroes, signs, trailing decorations, overflow, and extra separators
/// are rejected without allocation.
[[nodiscard]] inline canonical_generation_sidecar_name
parse_canonical_generation_sidecar_name(std::string_view relation_name,
                                        std::string_view prefix) noexcept {
    if (!relation_name.starts_with(prefix))
        return {};
    const std::string_view suffix = relation_name.substr(prefix.size());
    const std::size_t separator = suffix.find('_');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= suffix.size() ||
        suffix.find('_', separator + 1) != std::string_view::npos) {
        return {};
    }
    const std::string_view bucket_text = suffix.substr(0, separator);
    const std::string_view shard_text = suffix.substr(separator + 1);
    if ((bucket_text.size() > 1 && bucket_text.front() == '0') ||
        (shard_text.size() > 1 && shard_text.front() == '0')) {
        return {};
    }
    const auto parse_decimal =
        [](std::string_view text, std::uint64_t maximum, std::uint64_t& result) noexcept {
            result = 0;
            for (const char character : text) {
                if (character < '0' || character > '9')
                    return false;
                const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
                if (result > (maximum - digit) / 10u)
                    return false;
                result = result * 10u + digit;
            }
            return true;
        };
    std::uint64_t bucket = 0;
    std::uint64_t shard = 0;
    if (!parse_decimal(bucket_text, (std::numeric_limits<std::uint8_t>::max)(), bucket) ||
        !parse_decimal(shard_text, (std::numeric_limits<std::uint32_t>::max)(), shard)) {
        return {};
    }
    return canonical_generation_sidecar_name{
        .valid = true,
        .bucket_idx = static_cast<std::uint8_t>(bucket),
        .shard_idx = static_cast<std::uint32_t>(shard),
    };
}

/// Execute one generation-clear DDL statement through a checked SPI lifetime.
/// The activity bit stays set if PostgreSQL jumps out of `SPI_execute`, allowing
/// the enclosing error path to finish the connection.
inline void run_checked_generation_clear_ddl(const char* ddl, bool& spi_active) {
    if (ddl == nullptr || *ddl == '\0' || spi_active) {
        minimizer_generation_storage_error("generation clear DDL state is invalid");
    }
    const int connect_rc = SPI_connect();
    if (connect_rc != SPI_OK_CONNECT) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq current-generation index: generation clear DDL SPI_connect failed"),
                 errdetail("SPI rc=%d", connect_rc)));
    }
    spi_active = true;
    const int execute_rc = SPI_execute(ddl, /*read_only=*/false, /*tcount=*/0);
    const int finish_rc = SPI_finish();
    spi_active = finish_rc != SPI_OK_FINISH;
    if (execute_rc != SPI_OK_UTILITY || finish_rc != SPI_OK_FINISH) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq current-generation index: generation clear DDL failed"),
                 errdetail("execute_rc=%d finish_rc=%d", execute_rc, finish_rc)));
    }
}

/**
 * @brief Resolves or creates the canonical minimizer-generation sidecar relation.
 *
 * The source relation is validated under `AccessShareLock`. A valid existing
 * sidecar keeps its relation lock until transaction end, and creation is
 * serialized by the index advisory transaction lock.
 *
 * @return OID of the validated unlogged generation relation.
 */
[[nodiscard]] Oid fragment_index_storage::ensure_minimizer_generation_storage() {
    if (index_name_ != k_minimizers_per_fragment_index_name &&
        index_name_ != k_minimizers_per_sequence_index_name) {
        minimizer_generation_storage_error(
            "generation storage is only valid for minimizer indexes");
    }
    if (db_oid_ != MyDatabaseId || !OidIsValid(user_relid_) || !OidIsValid(tablespace_oid_) ||
        !OidIsValid(main_relfilenode_) || column_attno_ <= 0) {
        minimizer_generation_storage_error("generation storage identity is invalid");
    }
    acquire_index_advisory_xact_lock(
        db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, index_name_);

    Relation source_relation =
        bioseq::postgres::support::try_open_postgres_relation(user_relid_, AccessShareLock);
    if (source_relation == nullptr ||
        source_relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP ||
        source_relation->rd_locator.spcOid != tablespace_oid_) {
        if (source_relation != nullptr) {
            relation_close(source_relation, AccessShareLock);
        }
        minimizer_generation_storage_error(
            "generation storage requires a permanent/unlogged source in the resolved tablespace");
    }
    relation_close(source_relation, AccessShareLock);

    const std::string relation_name = this_table_name();
    const Oid internal_namespace = bioseq_internal_ns();
    const minimizer_generation_schema_expectation expected_schema{
        .namespace_oid = internal_namespace,
        .relation_name = relation_name,
        .tablespace_oid = tablespace_oid_,
    };
    index_relid_ = InvalidOid;
    Oid relation_oid = get_relname_relid(relation_name.c_str(), internal_namespace);

    if (OidIsValid(relation_oid)) {
        Relation relation =
            bioseq::postgres::support::try_open_postgres_relation(relation_oid, AccessShareLock);
        if (relation == nullptr || RelationGetNamespace(relation) != internal_namespace ||
            std::string_view{RelationGetRelationName(relation)} != relation_name) {
            if (relation != nullptr) {
                relation_close(relation, AccessShareLock);
            }
            minimizer_generation_storage_error(
                "generation relation disappeared during schema inspection");
        }
        const minimizer_generation_schema_inspection schema =
            inspect_minimizer_generation_schema(relation, nullptr, expected_schema);
        if (schema) {
            // Keep the AccessShare relation lock until transaction end so
            // external DDL cannot replace the validated tuple descriptor
            // before the row sink opens it.
            evict_generation_storage_residency_();
            relation_close(relation, NoLock);
            return relation_oid;
        }
        relation_close(relation, AccessShareLock);
        minimizer_generation_storage_error(
            fmt::format("existing generation relation requires explicit clear/reset: {}",
                        minimizer_generation_schema_error_name(schema.error)));
    }

    char* raw_tablespace_name = get_tablespace_name(tablespace_oid_);
    if (raw_tablespace_name == nullptr) {
        minimizer_generation_storage_error("resolved source tablespace no longer exists");
    }
    const std::string tablespace_name{raw_tablespace_name};
    pfree(raw_tablespace_name);
    const std::string primary_key_name = fmt::format(
        "seqidxpk_{:016x}_{:02x}_{:08x}",
        identity_hash64(db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, index_name_),
        static_cast<unsigned>(bucket_idx_),
        static_cast<unsigned>(shard_idx_));
    run_internal_ddl(fmt::format("CREATE UNLOGGED TABLE bioseq_internal.{} ("
                                 "query_key BIGINT NOT NULL, generation_id BIGINT NOT NULL, "
                                 "position_segment INTEGER NOT NULL, sub_key BIGINT NOT NULL, "
                                 "chunk_no INTEGER NOT NULL, chunk_bytes INTEGER NOT NULL, "
                                 "checksum BIGINT NOT NULL, blob BYTEA NOT NULL, "
                                 "CONSTRAINT {} PRIMARY KEY "
                                 "(generation_id,query_key,sub_key,position_segment,chunk_no) "
                                 "WITH (fillfactor=90) USING INDEX TABLESPACE {}) "
                                 "USING heap WITH (fillfactor=100) TABLESPACE {}",
                                 quote_identifier(relation_name.c_str()),
                                 quote_identifier(primary_key_name.c_str()),
                                 quote_identifier(tablespace_name.c_str()),
                                 quote_identifier(tablespace_name.c_str()))
                         .c_str());
    run_internal_ddl(fmt::format("ALTER TABLE bioseq_internal.{} "
                                 "ALTER COLUMN blob SET STORAGE PLAIN",
                                 quote_identifier(relation_name.c_str()))
                         .c_str());
    CommandCounterIncrement();

    relation_oid = get_relname_relid(relation_name.c_str(), internal_namespace);
    Relation created =
        OidIsValid(relation_oid)
            ? bioseq::postgres::support::try_open_postgres_relation(relation_oid, AccessShareLock)
            : nullptr;
    if (created == nullptr || RelationGetNamespace(created) != internal_namespace ||
        std::string_view{RelationGetRelationName(created)} != relation_name) {
        if (created != nullptr) {
            relation_close(created, AccessShareLock);
        }
        minimizer_generation_storage_error("canonical generation relation was not created");
    }
    const minimizer_generation_schema_inspection created_schema =
        inspect_minimizer_generation_schema(created, nullptr, expected_schema);
    if (!created_schema) {
        relation_close(created, AccessShareLock);
        minimizer_generation_storage_error(
            fmt::format("created generation relation schema mismatch: {}",
                        minimizer_generation_schema_error_name(created_schema.error)));
    }
    relation_close(created, NoLock);
    CacheInvalidateRelcacheByRelid(relation_oid);
    CacheInvalidateRelcacheByRelid(created_schema.primary_index_oid);
    // Broadcast on the owning relation because every residency key carries
    // `user_relid`, not the physical sidecar OID. Other backends can evict
    // cached bytes after this first canonical sidecar creation commits.
    CacheInvalidateRelcacheByRelid(user_relid_);
    evict_generation_storage_residency_();
    return relation_oid;
}

/**
 * @brief Drops every owned generation sidecar and clears its age metadata.
 *
 * Candidate relations are accepted only after canonical-name, shard-ownership,
 * physical-identity, and schema checks. Each accepted relation is locked with
 * `AccessExclusiveLock` before deletion, and cleanup contains PostgreSQL errors
 * so no live C++ owner crosses the longjmp boundary.
 *
 * @return Counts of dropped relations and deleted metadata rows.
 */
[[nodiscard]] minimizer_generation_clear_result
fragment_index_storage::clear_minimizer_generation_storage() {
    // Identifies one discovered sidecar before it is admitted for deletion.
    struct physical_relation {
        Oid oid = InvalidOid;
        std::string name;
        std::uint8_t bucket_idx = 0;
        std::uint32_t shard_idx = 0;
    };
    // Owns SPI, relation, allocation, and exception state across PG_TRY cleanup.
    struct clear_state {
        bool spi_active = false;
        Relation locked_relation = nullptr;
        LOCKMODE locked_relation_mode = NoLock;
        char* spi_text = nullptr;
        char* quoted_literal = nullptr;
        Oid internal_namespace = InvalidOid;
        std::string index_name;
        std::string prefix;
        std::string owned_shards_sql;
        std::string discover_sql;
        std::string ddl_sql;
        std::vector<std::pair<std::uint8_t, std::uint32_t>> owned_shards;
        std::vector<physical_relation> candidates;
        std::vector<physical_relation> relations;
        std::vector<physical_relation> verification_candidates;
        minimizer_generation_clear_result result{};
        std::exception_ptr cpp_error;
        bool completed = false;
    };
    auto* state = new (std::nothrow) clear_state{};
    if (state == nullptr) {
        ereport(
            ERROR,
            (errcode(ERRCODE_OUT_OF_MEMORY),
             errmsg("bioseq current-generation index: generation clear state allocation failed")));
    }
    MemoryContext saved_context = CurrentMemoryContext;
    ErrorData* volatile postgres_error = nullptr;

    PG_TRY();
    {
        try {
            if (index_name_ != k_minimizers_per_fragment_index_name &&
                index_name_ != k_minimizers_per_sequence_index_name) {
                minimizer_generation_storage_error(
                    "generation clear is only valid for minimizer indexes");
            }
            if (db_oid_ != MyDatabaseId || !OidIsValid(user_relid_) ||
                !OidIsValid(tablespace_oid_) || !OidIsValid(main_relfilenode_) ||
                column_attno_ <= 0) {
                minimizer_generation_storage_error("generation clear identity is invalid");
            }

            acquire_index_advisory_xact_lock(
                db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, index_name_);

            // A failed operation or enclosing savepoint rollback retains
            // no stale local object; the next access resolves by name.
            index_relid_ = InvalidOid;
            chunk_payload_bytes_.clear();
            release_runs();
            evict_generation_storage_residency_();

            state->prefix = fmt::format(
                "seqidx_{}_{:016x}_",
                short_index_for(index_name_),
                identity_hash64(
                    db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, index_name_));
            state->index_name.assign(index_name_);
            state->internal_namespace = bioseq_internal_ns();
            state->owned_shards_sql = fmt::format("SELECT bucket_idx::integer, shard_idx "
                                                  "FROM bioseq_internal.seq_sub_table "
                                                  "WHERE database_oid={}::oid "
                                                  "AND tablespace_oid={}::oid "
                                                  "AND main_relfilenode={}::oid "
                                                  "AND column_attno={}::smallint "
                                                  "ORDER BY bucket_idx, shard_idx",
                                                  static_cast<unsigned>(db_oid_),
                                                  static_cast<unsigned>(tablespace_oid_),
                                                  static_cast<unsigned>(main_relfilenode_),
                                                  static_cast<int>(column_attno_));
            state->quoted_literal = quote_literal_cstr(state->prefix.c_str());
            state->discover_sql = fmt::format("SELECT c.oid, c.relname::text FROM pg_class c "
                                              "WHERE c.relnamespace={}::oid "
                                              "AND c.relkind='r' "
                                              "AND left(c.relname::text, length({}))={} "
                                              "ORDER BY c.oid",
                                              static_cast<unsigned>(state->internal_namespace),
                                              state->quoted_literal,
                                              state->quoted_literal);
            pfree(state->quoted_literal);
            state->quoted_literal = nullptr;

            const int discovery_connect_rc = SPI_connect();
            if (discovery_connect_rc != SPI_OK_CONNECT) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq current-generation index: generation clear discovery "
                                "SPI_connect failed"),
                         errdetail("SPI rc=%d", discovery_connect_rc)));
            }
            state->spi_active = true;
            const int owned_rc = SPI_execute(state->owned_shards_sql.c_str(), true, 0);
            if (owned_rc != SPI_OK_SELECT) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq current-generation index: generation clear owned-shard "
                                "discovery failed"),
                         errdetail("SPI rc=%d", owned_rc)));
            }
            state->owned_shards.reserve(static_cast<std::size_t>(SPI_processed));
            for (std::uint64_t row = 0; row < SPI_processed; ++row) {
                bool bucket_is_null = false;
                bool shard_is_null = false;
                const std::int32_t bucket = DatumGetInt32(SPI_getbinval(
                    SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1, &bucket_is_null));
                const std::int32_t shard = DatumGetInt32(SPI_getbinval(
                    SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 2, &shard_is_null));
                if (bucket_is_null || shard_is_null || bucket < 0 ||
                    bucket > (std::numeric_limits<std::uint8_t>::max)() || shard < 0) {
                    minimizer_generation_storage_error(
                        "generation clear found an invalid owned shard identity");
                }
                state->owned_shards.emplace_back(static_cast<std::uint8_t>(bucket),
                                                 static_cast<std::uint32_t>(shard));
            }

            const int discover_rc = SPI_execute(state->discover_sql.c_str(), true, 0);
            if (discover_rc != SPI_OK_SELECT) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq current-generation index: generation clear relation "
                                "discovery failed"),
                         errdetail("SPI rc=%d", discover_rc)));
            }
            state->candidates.reserve(static_cast<std::size_t>(SPI_processed));
            for (std::uint64_t row = 0; row < SPI_processed; ++row) {
                bool oid_is_null = false;
                const Datum oid_datum =
                    SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1, &oid_is_null);
                state->spi_text = SPI_getvalue(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 2);
                if (oid_is_null || state->spi_text == nullptr) {
                    minimizer_generation_storage_error(
                        "generation clear discovery returned a NULL identity");
                }
                state->candidates.emplace_back();
                physical_relation& candidate = state->candidates.back();
                candidate.oid = DatumGetObjectId(oid_datum);
                candidate.name.assign(state->spi_text);
                pfree(state->spi_text);
                state->spi_text = nullptr;
                if (!OidIsValid(candidate.oid) || !candidate.name.starts_with(state->prefix)) {
                    minimizer_generation_storage_error(
                        "generation clear discovery escaped its exact prefix");
                }
            }
            const int discovery_finish_rc = SPI_finish();
            state->spi_active = discovery_finish_rc != SPI_OK_FINISH;
            if (discovery_finish_rc != SPI_OK_FINISH) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq current-generation index: generation clear discovery "
                                "SPI_finish failed"),
                         errdetail("SPI rc=%d", discovery_finish_rc)));
            }

            // A broad prefix match is only a candidate.  Ownership also
            // requires canonical decimal spelling, an exact source shard
            // catalog identity, and the exact generation tuple schema.
            for (std::size_t candidate_index = 0; candidate_index < state->candidates.size();
                 ++candidate_index) {
                physical_relation& candidate = state->candidates[candidate_index];
                const canonical_generation_sidecar_name parsed =
                    parse_canonical_generation_sidecar_name(candidate.name, state->prefix);
                if (!parsed.valid)
                    continue;
                bool owned = false;
                for (const auto& shard : state->owned_shards) {
                    if (shard.first == parsed.bucket_idx && shard.second == parsed.shard_idx) {
                        owned = true;
                        break;
                    }
                }
                if (!owned)
                    continue;
                state->locked_relation = bioseq::postgres::support::try_open_postgres_relation(
                    candidate.oid, AccessShareLock);
                state->locked_relation_mode = AccessShareLock;
                if (state->locked_relation == nullptr ||
                    get_relname_relid(candidate.name.c_str(), state->internal_namespace) !=
                        candidate.oid ||
                    RelationGetNamespace(state->locked_relation) != state->internal_namespace ||
                    std::string_view{RelationGetRelationName(state->locked_relation)} !=
                        candidate.name) {
                    minimizer_generation_storage_error(
                        "generation clear candidate identity changed during inspection");
                }
                const minimizer_generation_schema_inspection schema =
                    inspect_minimizer_generation_schema(state->locked_relation);
                relation_close(state->locked_relation, AccessShareLock);
                state->locked_relation = nullptr;
                state->locked_relation_mode = NoLock;
                if (!schema)
                    continue;
                candidate.bucket_idx = parsed.bucket_idx;
                candidate.shard_idx = parsed.shard_idx;
                state->relations.push_back(candidate);
            }

            for (std::size_t relation_index = 0; relation_index < state->relations.size();
                 ++relation_index) {
                physical_relation& target = state->relations[relation_index];
                state->locked_relation = bioseq::postgres::support::try_open_postgres_relation(
                    target.oid, AccessExclusiveLock);
                state->locked_relation_mode = AccessExclusiveLock;
                if (state->locked_relation == nullptr ||
                    get_relname_relid(target.name.c_str(), state->internal_namespace) !=
                        target.oid ||
                    RelationGetNamespace(state->locked_relation) != state->internal_namespace ||
                    std::string_view{RelationGetRelationName(state->locked_relation)} !=
                        target.name) {
                    minimizer_generation_storage_error(
                        "generation relation identity changed while acquiring clear lock");
                }
                const minimizer_generation_schema_inspection locked_schema =
                    inspect_minimizer_generation_schema(state->locked_relation);
                if (!locked_schema) {
                    minimizer_generation_storage_error(
                        "generation relation schema changed while acquiring clear lock");
                }
                const Oid primary_index = locked_schema.primary_index_oid;
                CacheInvalidateRelcacheByRelid(target.oid);
                CacheInvalidateRelcacheByRelid(primary_index);
                relation_close(state->locked_relation, NoLock);
                state->locked_relation = nullptr;
                state->locked_relation_mode = NoLock;

                state->ddl_sql.assign("DROP TABLE bioseq_internal.");
                state->ddl_sql.append(quote_identifier(target.name.c_str()));
                run_checked_generation_clear_ddl(state->ddl_sql.c_str(), state->spi_active);
                CommandCounterIncrement();
                if (OidIsValid(get_relname_relid(target.name.c_str(), state->internal_namespace))) {
                    minimizer_generation_storage_error(
                        "generation clear DDL retained an owned relation");
                }
            }
            state->result.dropped_relation_count =
                static_cast<std::uint64_t>(state->relations.size());

            state->result.deleted_metadata_age_rows = delete_metadata_age_log_pg(
                user_relid_, main_relfilenode_, column_attno_, state->index_name);
            CommandCounterIncrement();

            // Re-discover broadly, but fail only if a canonical,
            // catalog-owned, exact generation relation survived.  Benign
            // near-prefix relations are deliberately ignored.
            const int verify_connect_rc = SPI_connect();
            if (verify_connect_rc != SPI_OK_CONNECT) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq current-generation index: generation clear verification "
                                "SPI_connect failed"),
                         errdetail("SPI rc=%d", verify_connect_rc)));
            }
            state->spi_active = true;
            const int verify_rc = SPI_execute(state->discover_sql.c_str(), true, 0);
            if (verify_rc != SPI_OK_SELECT) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq current-generation index: generation clear verification "
                                "query failed"),
                         errdetail("SPI rc=%d", verify_rc)));
            }
            state->verification_candidates.reserve(static_cast<std::size_t>(SPI_processed));
            for (std::uint64_t row = 0; row < SPI_processed; ++row) {
                bool oid_is_null = false;
                const Datum oid_datum =
                    SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1, &oid_is_null);
                state->spi_text = SPI_getvalue(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 2);
                if (oid_is_null || state->spi_text == nullptr) {
                    minimizer_generation_storage_error(
                        "generation clear verification returned a NULL identity");
                }
                state->verification_candidates.emplace_back();
                physical_relation& candidate = state->verification_candidates.back();
                candidate.oid = DatumGetObjectId(oid_datum);
                candidate.name.assign(state->spi_text);
                pfree(state->spi_text);
                state->spi_text = nullptr;
            }
            const int verify_finish_rc = SPI_finish();
            state->spi_active = verify_finish_rc != SPI_OK_FINISH;
            if (verify_finish_rc != SPI_OK_FINISH) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq current-generation index: generation clear verification "
                                "SPI_finish failed"),
                         errdetail("SPI rc=%d", verify_finish_rc)));
            }
            for (std::size_t candidate_index = 0;
                 candidate_index < state->verification_candidates.size();
                 ++candidate_index) {
                physical_relation& candidate = state->verification_candidates[candidate_index];
                const canonical_generation_sidecar_name parsed =
                    parse_canonical_generation_sidecar_name(candidate.name, state->prefix);
                if (!parsed.valid)
                    continue;
                bool owned = false;
                for (const auto& shard : state->owned_shards) {
                    if (shard.first == parsed.bucket_idx && shard.second == parsed.shard_idx) {
                        owned = true;
                        break;
                    }
                }
                if (!owned)
                    continue;
                state->locked_relation = bioseq::postgres::support::try_open_postgres_relation(
                    candidate.oid, AccessShareLock);
                if (state->locked_relation == nullptr) {
                    // The discovery snapshot may still name a relation
                    // that the preceding DROP has already removed.  This
                    // verifier rejects only an exact surviving owned
                    // sidecar; disappearance is the desired state.
                    state->locked_relation_mode = NoLock;
                    continue;
                }
                state->locked_relation_mode = AccessShareLock;
                const bool exact_identity =
                    RelationGetNamespace(state->locked_relation) == state->internal_namespace &&
                    std::string_view{RelationGetRelationName(state->locked_relation)} ==
                        candidate.name;
                const minimizer_generation_schema_inspection schema =
                    inspect_minimizer_generation_schema(state->locked_relation);
                relation_close(state->locked_relation, AccessShareLock);
                state->locked_relation = nullptr;
                state->locked_relation_mode = NoLock;
                if (exact_identity && schema) {
                    minimizer_generation_storage_error(
                        "generation clear verification retained an owned relation");
                }
            }

            index_relid_ = InvalidOid;
            chunk_payload_bytes_.clear();
            release_runs();
            evict_generation_storage_residency_();
            CacheInvalidateRelcacheByRelid(user_relid_);
            state->completed = true;
        } catch (...) {
            state->cpp_error = std::current_exception();
        }
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        ErrorData* volatile saved_error = CopyErrorData();
        FlushErrorState();
        if (state->spi_text != nullptr) {
            PG_TRY();
            {
                pfree(state->spi_text);
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
            state->spi_text = nullptr;
        }
        if (state->quoted_literal != nullptr) {
            PG_TRY();
            {
                pfree(state->quoted_literal);
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
            state->quoted_literal = nullptr;
        }
        if (state->locked_relation != nullptr) {
            Relation cleanup = state->locked_relation;
            const LOCKMODE cleanup_mode = state->locked_relation_mode;
            state->locked_relation = nullptr;
            state->locked_relation_mode = NoLock;
            PG_TRY();
            {
                relation_close(cleanup, cleanup_mode);
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
        }
        if (state->spi_active) {
            state->spi_active = false;
            PG_TRY();
            {
                (void)SPI_finish();
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
        }
        postgres_error = saved_error;
    }
    PG_END_TRY();

    if (postgres_error != nullptr) {
        ErrorData* exact_error = postgres_error;
        delete state;
        ReThrowError(exact_error);
    }

    if (state->cpp_error != nullptr) {
        if (state->spi_text != nullptr) {
            PG_TRY();
            {
                pfree(state->spi_text);
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
            state->spi_text = nullptr;
        }
        if (state->quoted_literal != nullptr) {
            PG_TRY();
            {
                pfree(state->quoted_literal);
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
            state->quoted_literal = nullptr;
        }
        if (state->locked_relation != nullptr) {
            Relation cleanup = state->locked_relation;
            const LOCKMODE cleanup_mode = state->locked_relation_mode;
            state->locked_relation = nullptr;
            state->locked_relation_mode = NoLock;
            PG_TRY();
            {
                relation_close(cleanup, cleanup_mode);
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
        }
        if (state->spi_active) {
            state->spi_active = false;
            PG_TRY();
            {
                (void)SPI_finish();
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
        }
        char cpp_error_detail[512] = "unknown C++ exception";
        try {
            std::rethrow_exception(state->cpp_error);
        } catch (const std::exception& error) {
            const char* message = error.what();
            strlcpy(cpp_error_detail,
                    message == nullptr ? "std::exception without detail" : message,
                    sizeof(cpp_error_detail));
        } catch (...) {
            strlcpy(cpp_error_detail, "unknown C++ exception", sizeof(cpp_error_detail));
        }
        state->cpp_error = nullptr;
        delete state;
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq current-generation index: generation clear failed"),
                 errdetail("C++ error=%s", cpp_error_detail)));
    }
    if (!state->completed) {
        delete state;
        minimizer_generation_storage_error("generation clear did not complete or report an error");
    }
    const minimizer_generation_clear_result result = state->result;
    delete state;
    return result;
}

/**
 * @brief Opens a snapshot-bound reader for the handle's generation key.
 * @param[in] query_key Logical key fixed when this storage handle was opened.
 * @param[in] executor_snapshot Registered executor snapshot borrowed by the session.
 * @param[in] expected Expected immutable generation identity.
 * @return A read session that borrows this handle and the registered snapshot.
 */
[[nodiscard]] fragment_index_storage::generation_read_session
fragment_index_storage::begin_generation_read(
    std::uint64_t query_key,
    Snapshot executor_snapshot,
    expected_minimizer_generation_identity expected) const {
    if (query_key != query_key_) {
        minimizer_generation_storage_error("generation read changed the handle query key");
    }
    return generation_read_session{this_table_name(),
                                   query_key,
                                   executor_snapshot,
                                   expected,
                                   tablespace_oid_,
                                   minimizer_generation_snapshot_registration::borrow_registered};
}

/**
 * @brief Opens a grouped-SoA row sink for one new generation.
 * @param[in] query_key Logical key fixed when this storage handle was opened.
 * @param[in] generation_id Immutable identifier assigned to the generation.
 * @return A sink bound to the validated canonical generation relation.
 */
[[nodiscard]] fragment_index_storage::generation_row_sink
fragment_index_storage::begin_generation_write(std::uint64_t query_key,
                                               std::uint64_t generation_id) {
    if (query_key != query_key_) {
        minimizer_generation_storage_error("generation writer changed the handle query key");
    }
    (void)ensure_minimizer_generation_storage();
    return generation_row_sink{minimizer_generation_administration_identity{
                                   .database_oid = db_oid_,
                                   .tablespace_oid = tablespace_oid_,
                                   .main_relfilenode = main_relfilenode_,
                                   .user_relid = user_relid_,
                                   .column_attno = column_attno_,
                                   .index_name = std::string{index_name_},
                                   .table_name = this_table_name(),
                               },
                               query_key,
                               generation_id};
}

/**
 * @brief Opens a hash-major writer for one new minimizer generation.
 * @param[in] query_key Logical key fixed when this storage handle was opened.
 * @param[in] generation_id Immutable identifier assigned to the generation.
 * @return A writer bound to the validated canonical generation relation.
 */
[[nodiscard]] fragment_index_storage::hash_major_minimizer_generation_writer
fragment_index_storage::begin_hash_major_minimizer_generation_write(std::uint64_t query_key,
                                                                    std::uint64_t generation_id) {
    if (query_key != query_key_) {
        minimizer_generation_storage_error("hash-major writer changed the handle query key");
    }
    (void)ensure_minimizer_generation_storage();
    return hash_major_minimizer_generation_writer{minimizer_generation_administration_identity{
                                                      .database_oid = db_oid_,
                                                      .tablespace_oid = tablespace_oid_,
                                                      .main_relfilenode = main_relfilenode_,
                                                      .user_relid = user_relid_,
                                                      .column_attno = column_attno_,
                                                      .index_name = std::string{index_name_},
                                                      .table_name = this_table_name(),
                                                  },
                                                  query_key,
                                                  generation_id};
}

/**
 * @brief Resolves the direct-fragment relation and optionally creates it.
 *
 * Cached OIDs are accepted only when the current namespace lookup still maps
 * the canonical name to the same OID, which keeps handles valid across
 * subtransaction rollback.
 *
 * @param[in] create_if_missing Whether an absent relation may be created.
 * @return Current relation OID, or `InvalidOid` when absent and creation is disabled.
 */
[[nodiscard]] Oid fragment_index_storage::resolve_relid_(bool create_if_missing) const {
    const std::string nm = this_table_name();
    const Oid ns = bioseq_internal_ns();
    const char* relation_name = nm.c_str();
    Oid rid = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation_name, ns] { return get_relname_relid(relation_name, ns); });
    // The C++ handle can outlive a PostgreSQL subtransaction.  Never trust
    // a cached OID unless the authoritative namespace/name lookup still
    // resolves to exactly that OID; rollback may have restored a prior
    // relation or removed a relation created below a SAVEPOINT.
    if (OidIsValid(index_relid_) && index_relid_ == rid) {
        return index_relid_;
    }
    index_relid_ = InvalidOid;
    if (!OidIsValid(rid) && create_if_missing) {
        const Oid user_relid = user_relid_;
        const char* persistence = bioseq::postgres::support::invoke_pg_as_cpp(
            [user_relid] { return persistence_kw_for(user_relid); });
        const char* quoted_name_pg = bioseq::postgres::support::invoke_pg_as_cpp(
            [relation_name] { return quote_identifier(relation_name); });
        const bool quoted_name_is_copy = quoted_name_pg != relation_name;
        std::string quoted_name;
        try {
            quoted_name.assign(quoted_name_pg);
        } catch (...) {
            if (quoted_name_is_copy) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [quoted_name_pg] { pfree(const_cast<char*>(quoted_name_pg)); });
            }
            throw;
        }
        if (quoted_name_is_copy) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [quoted_name_pg] { pfree(const_cast<char*>(quoted_name_pg)); });
        }
        const std::string create_ddl = fmt::format(
            "CREATE {} TABLE IF NOT EXISTS bioseq_internal.{} ("
            "query_key BIGINT NOT NULL, sub_key BIGINT NOT NULL, chunk_no INT NOT NULL, "
            "chunk_bytes INT NOT NULL, blob BYTEA NOT NULL, "
            "PRIMARY KEY (query_key, sub_key, chunk_no)) USING heap;",
            persistence,
            quoted_name);
        const char* create_ddl_data = create_ddl.c_str();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [create_ddl_data] { run_internal_ddl(create_ddl_data); });
        const std::string storage_ddl = fmt::format(
            "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob SET STORAGE EXTERNAL;", quoted_name);
        const char* storage_ddl_data = storage_ddl.c_str();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [storage_ddl_data] { run_internal_ddl(storage_ddl_data); });
        rid = bioseq::postgres::support::invoke_pg_as_cpp(
            [relation_name, ns] { return get_relname_relid(relation_name, ns); });
        if (!OidIsValid(rid))
            index_error("failed to create seqidx table");
    }
    index_relid_ = rid;
    return rid;
}

/**
 * @brief Opens and validates the direct-fragment heap relation.
 *
 * The caller owns the returned relation and its requested lock. On any schema
 * or identity mismatch this function closes the relation before raising an error.
 *
 * @param[in] create_if_missing Whether an absent direct relation may be created.
 * @param[in] lock PostgreSQL lock mode acquired for the returned relation.
 * @return Open relation, or null when it is absent and creation is disabled.
 */
[[nodiscard]] Relation fragment_index_storage::open_index_relation_(bool create_if_missing,
                                                                    LOCKMODE lock) const {
    const Oid rid = resolve_relid_(create_if_missing);
    if (!OidIsValid(rid))
        return nullptr;
    const std::string expected_name = this_table_name();
    constexpr std::array<minimizer_generation_index_attribute_descriptor, 5> k_fragment_descriptors{
        {
            {k_attribute_query_key, "query_key", INT8OID},
            {k_attribute_sub_key, "sub_key", INT8OID},
            {k_attribute_chunk_number, "chunk_no", INT4OID},
            {k_attribute_chunk_bytes, "chunk_bytes", INT4OID},
            {k_attribute_blob, "blob", BYTEAOID},
        }};
    const Oid expected_namespace = bioseq_internal_ns();
    const char* expected_name_data = expected_name.c_str();
    const minimizer_generation_index_attribute_descriptor* descriptors =
        k_fragment_descriptors.data();
    const std::size_t descriptor_count = k_fragment_descriptors.size();
    fragment_read_resources resources{lock};
    try {
        resources.relation = bioseq::postgres::support::invoke_pg_as_cpp([rid, lock] {
            return bioseq::postgres::support::try_open_postgres_relation(rid, lock);
        });
        if (resources.relation == nullptr) {
            return resources.release_relation();
        }

        // Return 1 for a physical-identity mismatch and 2 for a schema
        // mismatch. The leaf owns no C++ formatting state.
        const std::uint8_t invalid_reason =
            bioseq::postgres::support::invoke_pg_as_cpp([relation = resources.relation,
                                                         expected_namespace,
                                                         expected_name_data,
                                                         descriptors,
                                                         descriptor_count] {
                if (relation->rd_rel->relam != HEAP_TABLE_AM_OID ||
                    RelationGetNamespace(relation) != expected_namespace ||
                    std::strcmp(RelationGetRelationName(relation), expected_name_data) != 0) {
                    return std::uint8_t{1};
                }

                // This is the direct five-column surface. Never apply
                // these attnos to the generation relation.
                TupleDesc tuple_desc = RelationGetDescr(relation);
                if (tuple_desc == nullptr ||
                    tuple_desc->natts != static_cast<int>(descriptor_count)) {
                    return std::uint8_t{2};
                }
                for (std::size_t attribute_index = 0; attribute_index < descriptor_count;
                     ++attribute_index) {
                    const minimizer_generation_index_attribute_descriptor* expected =
                        descriptors + attribute_index;
                    Form_pg_attribute attribute =
                        TupleDescAttr(tuple_desc, static_cast<int>(attribute_index));
                    if (attribute->attnum != expected->attno || attribute->attisdropped ||
                        std::strcmp(NameStr(attribute->attname), expected->name.data()) != 0 ||
                        attribute->atttypid != expected->type_oid || !attribute->attnotnull) {
                        return std::uint8_t{2};
                    }
                }
                return std::uint8_t{0};
            });
        if (invalid_reason != 0u) {
            index_relid_ = InvalidOid;
            resources.close_checked();
            const std::string error =
                invalid_reason == 1u
                    ? fmt::format("{} has the wrong physical identity", expected_name)
                    : fmt::format("fragment API refuses an incompatible schema for {}",
                                  expected_name);
            index_error(error);
        }
        return resources.release_relation();
    } catch (const bioseq::postgres::support::postgres_error&) {
        resources.abandon_after_pg_error_noexcept();
        throw;
    } catch (...) {
        const std::exception_ptr error = std::current_exception();
        try {
            resources.close_checked();
        } catch (...) {
            resources.abandon_after_pg_error_noexcept();
            throw;
        }
        std::rethrow_exception(error);
    }
}

} // namespace bioseq::storage::indexing
