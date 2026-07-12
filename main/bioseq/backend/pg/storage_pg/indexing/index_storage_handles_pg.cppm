/**
 * @file index_storage_handles_pg.cppm
 * @brief Lightweight Postgres index-storage handle factories and active-read
 *        policy resolver. Kept separate from index_admin_pg.cppm so scheduler
 *        pipeline code does not import the broad admin/update_index registry.
 */
module;
#include "common.h"
#include "access/relation.h"
#include "utils/memutils.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

export module bioseq.storage.indexing_pg.handles;

import bioseq.common.mlogging;
import bioseq.pg_utils;
import bioseq.config;                  // DOC0322 §9.3 — bioseq_index_run_buffer_bytes GUC
import bioseq.storage.manager;
import bioseq.indexing.type;
import bioseq.storage.indexing_pg.chunked_storage;

export namespace bioseq::storage::indexing {

using bioseq::indexing::SequenceIndexType;
using bioseq::indexing::index_sub_key_t;
using bioseq::indexing::DIRECTORY_SUB_KEY;

struct RelationIdentity { Oid db_oid, tablespace_oid, relfilenode; };

[[nodiscard]] inline RelationIdentity index_relation_identity(Oid user_relid) {
    RelationIdentity id{ MyDatabaseId, InvalidOid, InvalidOid };
    Relation r = bioseq::pg_utils::invoke_pg_as_cpp(
        [user_relid] {
            return try_safe_relation_open(user_relid, AccessShareLock);
        });
    if (r != nullptr) {
        id.tablespace_oid = OidIsValid(r->rd_rel->reltablespace) ? r->rd_rel->reltablespace : MyDatabaseTableSpace;
        id.relfilenode = (Oid)r->rd_locator.relNumber;
        if (!OidIsValid(id.relfilenode)) id.relfilenode = RelationGetRelid(r);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [r] { relation_close(r, AccessShareLock); });
    }
    return id;
}

[[nodiscard]] inline MemoryContext make_handle_ctx(MemoryContext parent, const char* /*tag*/) {
    MemoryContext owning_parent = parent ? parent : CurrentMemoryContext;
    return bioseq::pg_utils::invoke_pg_as_cpp([owning_parent] {
        return AllocSetContextCreate(
            owning_parent, "bioseq_index_handle", ALLOCSET_SMALL_SIZES);
    });
}

[[nodiscard]] inline IndexStorageFragmentPg open_fragment_storage(
        Oid user_relid, Oid db, Oid spc, Oid relfn, std::int16_t attno, std::string_view index_name,
        const seq_locator& loc, std::uint64_t sequence_rowno, seq_caches* caches, MemoryContext mctx) {
    // The returned handle owns an open child MemoryContext. Normal callers
    // must invoke close_checked(); failure paths must invoke
    // abandon_after_pg_error_noexcept() before destruction. The destructor is
    // deliberately zero-PostgreSQL and asserts this ownership transition.
    IndexStorageFragmentPg s;
    s.db_oid_=db; s.tablespace_oid_=spc; s.main_relfilenode_=relfn; s.user_relid_=user_relid;
    s.read_caches_=caches; s.column_attno_=attno; s.index_name_=index_name;
    s.bucket_idx_=(std::uint8_t)loc.bucket_idx; s.shard_idx_=loc.shard_idx; s.query_key_=sequence_rowno;
    s.own_mctx_ = make_handle_ctx(mctx, "idx_fragment_handle");
    s.run_buffer_override_bytes_ = bioseq_index_run_buffer_bytes > 0 ? (std::size_t)bioseq_index_run_buffer_bytes : 0;
    return s;
}

[[nodiscard]] inline IndexStorageSequencePg open_sequence_storage(
        Oid user_relid, Oid db, Oid spc, Oid relfn, std::int16_t attno, std::string_view index_name,
        const seq_locator& loc, std::uint64_t sequence_rowno, seq_caches* caches, MemoryContext mctx) {
    // Same explicit ownership contract as open_fragment_storage: checked
    // close on success, zero-PG abandonment before failure destruction.
    IndexStorageSequencePg s;
    s.db_oid_=db; s.tablespace_oid_=spc; s.main_relfilenode_=relfn; s.user_relid_=user_relid;
    s.read_caches_=caches; s.column_attno_=attno; s.index_name_=index_name;
    s.bucket_idx_=(std::uint8_t)loc.bucket_idx; s.shard_idx_=loc.shard_idx; s.query_key_=sequence_rowno;
    s.own_mctx_ = make_handle_ctx(mctx, "idx_sequence_handle");
    return s;
}

[[nodiscard]] inline IndexStorageTablePg open_table_storage(
        Oid user_relid, Oid db, Oid spc, Oid relfn, std::int16_t attno, std::string_view index_name,
        seq_caches* caches, MemoryContext mctx) {
    // Same explicit ownership contract as open_fragment_storage: checked
    // close on success, zero-PG abandonment before failure destruction.
    IndexStorageTablePg s;
    s.db_oid_=db; s.tablespace_oid_=spc; s.main_relfilenode_=relfn; s.user_relid_=user_relid;
    s.read_caches_=caches; s.column_attno_=attno; s.index_name_=index_name;
    s.own_mctx_ = make_handle_ctx(mctx, "idx_table_handle");
    return s;
}

enum class ActiveReadPolicyResolutionSource : std::uint8_t {
	unresolved,
	non_relation,
    configured_row,
    authoritative_no_row_default,
};

[[nodiscard]] constexpr std::string_view
active_read_policy_resolution_source_name(
		const ActiveReadPolicyResolutionSource source) noexcept {
	switch (source) {
		case ActiveReadPolicyResolutionSource::unresolved:
			return "unresolved";
		case ActiveReadPolicyResolutionSource::non_relation:
			return "non_relation";
		case ActiveReadPolicyResolutionSource::configured_row:
			return "configured_row";
		case ActiveReadPolicyResolutionSource::authoritative_no_row_default:
			return "authoritative_no_row_default";
	}
	return "invalid";
}

static_assert(active_read_policy_resolution_source_name(
	ActiveReadPolicyResolutionSource::unresolved) == "unresolved");
static_assert(active_read_policy_resolution_source_name(
	ActiveReadPolicyResolutionSource::non_relation) == "non_relation");
static_assert(active_read_policy_resolution_source_name(
	ActiveReadPolicyResolutionSource::configured_row) == "configured_row");
static_assert(active_read_policy_resolution_source_name(
	ActiveReadPolicyResolutionSource::authoritative_no_row_default)
	== "authoritative_no_row_default");

struct ActiveReadPolicyResolution {
    std::string index_name;
    ActiveReadPolicyResolutionSource source =
		ActiveReadPolicyResolutionSource::unresolved;
};

struct ActiveReadPolicyResolutionCounters {
	std::uint64_t non_relation = 0;
    std::uint64_t configured_rows = 0;
    std::uint64_t authoritative_defaults = 0;
};

struct ActiveReadPolicyDispatchProofCounters {
	std::uint64_t configured_batches = 0;
	std::uint64_t authoritative_default_batches = 0;
	std::uint64_t non_relation_batches = 0;
};

struct ProhibitedExternalFileProducerEntryCounters {
	std::uint64_t constructor_entries = 0;
	std::uint64_t writer_entries = 0;
};

// The external-file producer is compile-disabled in the typed dispatcher.
// This exported immutable proof is queryable by the integration fixture.
inline constexpr ProhibitedExternalFileProducerEntryCounters
	g_prohibited_external_file_producer_entry_counters{};
static_assert(
	g_prohibited_external_file_producer_entry_counters.constructor_entries == 0);
static_assert(
	g_prohibited_external_file_producer_entry_counters.writer_entries == 0);

// PostgreSQL backends are single-threaded. These counters intentionally show
// whether a default came from an authoritative empty policy result rather
// than concealing an SPI failure as the old resolver did.
inline ActiveReadPolicyResolutionCounters
    g_active_read_policy_resolution_counters{};
inline ActiveReadPolicyDispatchProofCounters
	g_active_read_policy_dispatch_proof_counters{};
inline ActiveReadPolicyResolutionSource
	g_last_active_read_policy_dispatch_source =
		ActiveReadPolicyResolutionSource::unresolved;

inline void reset_active_read_policy_debug_state() noexcept {
	g_active_read_policy_resolution_counters = {};
	g_active_read_policy_dispatch_proof_counters = {};
	g_last_active_read_policy_dispatch_source =
		ActiveReadPolicyResolutionSource::unresolved;
}

[[noreturn]] inline void active_read_policy_error_as_cpp(
        std::string_view message,
        std::string_view detail = {}) {
    bioseq::pg_utils::invoke_pg_as_cpp([message, detail] {
        if (detail.empty()) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq active-read policy: %.*s",
                    static_cast<int>(message.size()), message.data())));
        }
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq active-read policy: %.*s",
                static_cast<int>(message.size()), message.data()),
             errdetail("%.*s", static_cast<int>(detail.size()),
                detail.data())));
    });
    pg_unreachable();
}

[[nodiscard]] inline ActiveReadPolicyResolution
resolve_active_read_index_outcome(
        Oid user_relid,
        Oid relfn,
        std::int16_t attno) {
	if (!OidIsValid(user_relid)) {
		++g_active_read_policy_resolution_counters.non_relation;
		bioseq_log<DBG>(
			"[bioseq] active-read policy non-relation transient shape index={}",
			bioseq::indexing::DEFAULT_ACTIVE_READ_INDEX);
		return ActiveReadPolicyResolution{
			.index_name = std::string{
				bioseq::indexing::DEFAULT_ACTIVE_READ_INDEX},
			.source = ActiveReadPolicyResolutionSource::non_relation,
		};
	}
	if (!OidIsValid(relfn) || attno <= 0) {
        active_read_policy_error_as_cpp(
			"relation-backed resolver identity is invalid",
			fmt::format("relation={} relfilenode={} attno={}",
				static_cast<unsigned>(user_relid),
				static_cast<unsigned>(relfn), static_cast<int>(attno)));
    }
    const int connect_status = bioseq::pg_utils::invoke_pg_as_cpp(
        [] { return SPI_connect(); });
    if (connect_status != SPI_OK_CONNECT) {
        active_read_policy_error_as_cpp(
            "SPI_connect failed",
            fmt::format("SPI rc={}", connect_status));
    }
    const std::string sql = fmt::format(
        "SELECT index_name FROM bioseq_internal.index_policy "
		"WHERE user_relid={}::oid AND main_relfilenode={}::oid AND column_attno={} AND is_active_read AND enabled",
        (unsigned)user_relid, (unsigned)relfn, (int)attno);
    const int execute_status = bioseq::pg_utils::invoke_pg_as_cpp(
        [&sql] { return SPI_execute(sql.c_str(), true, 1); });
    const std::uint64_t rows = execute_status == SPI_OK_SELECT
        ? static_cast<std::uint64_t>(SPI_processed) : 0;
    std::optional<std::string> configured_index;
    std::optional<std::string> semantic_error;
    if (execute_status != SPI_OK_SELECT) {
        semantic_error = fmt::format(
            "policy query returned SPI rc={}", execute_status);
    } else if (rows > 1) {
        semantic_error = fmt::format(
            "policy query returned {} rows", rows);
    } else if (rows == 1) {
        char* value = bioseq::pg_utils::invoke_pg_as_cpp([] {
            return SPI_getvalue(
                SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
        });
        if (value == nullptr || value[0] == '\0') {
            semantic_error = "configured policy row has no index name";
        } else {
            configured_index.emplace(value);
        }
        if (value != nullptr) {
            bioseq::pg_utils::invoke_pg_as_cpp([value] { pfree(value); });
        }
    }
    const int finish_status = bioseq::pg_utils::invoke_pg_as_cpp(
        [] { return SPI_finish(); });
    if (finish_status != SPI_OK_FINISH) {
        active_read_policy_error_as_cpp(
            "SPI_finish failed",
            fmt::format("SPI rc={}", finish_status));
    }
    if (semantic_error.has_value()) {
        active_read_policy_error_as_cpp(
            "policy query result is invalid", *semantic_error);
    }
    if (configured_index.has_value()) {
        ++g_active_read_policy_resolution_counters.configured_rows;
        bioseq_log<DBG>(
            "[bioseq] active-read policy configured relation={} relfilenode={} attno={} index={}",
            static_cast<unsigned>(user_relid),
            static_cast<unsigned>(relfn), static_cast<int>(attno),
            *configured_index);
        return ActiveReadPolicyResolution{
            .index_name = std::move(*configured_index),
            .source = ActiveReadPolicyResolutionSource::configured_row,
        };
    }
    ++g_active_read_policy_resolution_counters.authoritative_defaults;
    bioseq_log<DBG>(
        "[bioseq] active-read policy authoritative no-row default relation={} relfilenode={} attno={} index={}",
        static_cast<unsigned>(user_relid),
        static_cast<unsigned>(relfn), static_cast<int>(attno),
        bioseq::indexing::DEFAULT_ACTIVE_READ_INDEX);
    return ActiveReadPolicyResolution{
        .index_name = std::string{
            bioseq::indexing::DEFAULT_ACTIVE_READ_INDEX},
        .source = ActiveReadPolicyResolutionSource::
            authoritative_no_row_default,
    };
}

}  // namespace bioseq::storage::indexing
