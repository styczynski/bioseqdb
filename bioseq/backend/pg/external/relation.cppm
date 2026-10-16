/// @file
/// @brief Defines the read-only table access method for external relations.
module;
#include <bioseq/pg_compat.h>

#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>

extern "C" {
#include "access/reloptions.h"
#include "commands/defrem.h"
}

export module bioseq.postgres.external.relation;

import bioseq.postgres.storage.metadata.sql_rows;
import bioseq.postgres.storage.metadata.sql_row_codecs;

#if defined(BIOSEQ_EXTERNAL_PG) && BIOSEQ_EXTERNAL_PG
import bioseq.codegen.sql.annotation_model;

namespace bioseq::postgres::external::sql_routines {

extern "C" {
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
})]]
[[bioseq_sql::access_method]]
[[bioseq_sql::am_catalog_dependencies({
    .roots = bioseq::sql::entities<
        bioseq::storage::metadata::catalog_rows::external_relation_options>(),
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::table_am_handler,
})]]
/** @brief Returns the table access-method handler for external relations.
 *
 * PostgreSQL supplies the internal argument when it opens a table
 * using bioseq_external. Application SQL should query the external tables
 * instead of calling this handler. Those tables reject writes and local
 * indexes; their rows come from provider requests through BioseqExternalScan.
 * ```sql
 * -- Inspect the installed access method and its handler without contacting a provider.
 * SELECT amname, amhandler::regproc AS handler
 * FROM pg_catalog.pg_am
 * WHERE amname = 'bioseq_external';
 * ```
 */
PGDLLEXPORT Datum bioseq_external_handler(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::pg::internal,
    })]]
    FunctionCallInfo fcinfo);
}

} // namespace bioseq::postgres::external::sql_routines
#endif

export namespace bioseq::postgres::external {

/** @brief Type of rows returned by the table, selected by its entity  option. */
enum class relation_entity : int
{
	unspecified = 0,
	taxonomy,
	catalog,
	record_gene,
	record_location,
	sequence,
	sequence_chunk
};

/**
 * @brief Parsed PostgreSQL relation options.
 *
 * PostgreSQL requires vl_len_ first. The alphabet field stores an offset to
 * its string in the trailing bytes. The other options store their values inline.
 */
struct relation_options
{
	int32 vl_len_;
	int entity;
	int alphabet;
	int storage_width;
	int chunk_bases;
};

/** Parsed custom options removed from an external CREATE TABLE statement. */
struct create_relation_options
{
	bool applies;
	relation_entity entity;
	const char *alphabet;
	int storage_width;
	int chunk_bases;
};

/** @brief Parses a `WITH (...)` datum into PostgreSQL-owned relation options. */
[[nodiscard]] bytea *parse_relation_options(Datum options, bool validate);

/** Tests whether a CREATE TABLE explicitly selects bioseq_external. */
[[nodiscard]] bool is_external_create_statement(
	const CreateStmt *statement) noexcept;

/** Removes and parses the external table options before PostgreSQL runs CREATE TABLE. */
[[nodiscard]] create_relation_options consume_create_relation_options(
	CreateStmt *statement);

/** Saves the external table options in bioseq_internal.external_relation_options. */
void persist_create_relation_options(
	Oid relation_oid, const create_relation_options& options);

/**
 * Deletes the saved options for a table and returns whether, a row was removed.
 * A missing catalog returns false. If only the row is missing, missing_ok
 * decides whether to return false or raise an error.
 */
[[nodiscard]] bool delete_relation_options(
	Oid relation_oid, bool missing_ok);

/** @brief Reads the saved entity gption, raising an error if it is missing or invalid. */
[[nodiscard]] relation_entity entity_from_options(Relation relation);

/** @brief Returns the stable `TableAmRoutine` registered for external tables. */
[[nodiscard]] const TableAmRoutine *table_access_method() noexcept;

/** @brief Tests whether a relation uses the external table access method. */
[[nodiscard]] bool uses_table_access_method(Relation relation) noexcept;

/** @brief Returns the entity name used in SQL table options. */
[[nodiscard]] const char *to_string(relation_entity entity) noexcept;

} // namespace bioseq::postgres::external

/** Registers the external table access method's relation options. */
extern "C" void bioseq_external_tableam_init(void);

/* ======================================================================== */
/* Relation options                                                          */
/* ======================================================================== */

namespace bioseq::postgres::external {

namespace {

/* Assigned  during bioseq_external_tableam_init(). Zero means uninitialized. */
relopt_kind relation_option_kind = static_cast<relopt_kind>(0);

/* Acceptable `entity` values, null-terminated as the API requires. */
relopt_enum_elt_def entity_option_values[] = {
	{ "taxonomy",        static_cast<int>(relation_entity::taxonomy) },
	{ "catalog",         static_cast<int>(relation_entity::catalog) },
	{ "record_gene",     static_cast<int>(relation_entity::record_gene) },
	{ "record_location", static_cast<int>(relation_entity::record_location) },
	{ "sequence",        static_cast<int>(relation_entity::sequence) },
	{ "sequence_chunk",  static_cast<int>(relation_entity::sequence_chunk) },
	{ nullptr,           0 }
};

const relopt_parse_elt relation_option_fields[] = {
	{ "entity", RELOPT_TYPE_ENUM, offsetof(relation_options, entity) },
	{ "alphabet", RELOPT_TYPE_STRING, offsetof(relation_options, alphabet) },
	{ "storage_width", RELOPT_TYPE_INT,
	  offsetof(relation_options, storage_width) },
	{ "chunk_bases", RELOPT_TYPE_INT,
	  offsetof(relation_options, chunk_bases) },
};

/*
 * PostgreSQL calls this validator for supplied strings and the null default.
 * This callback accepts both and leaves the alphabet spelling unchanged.
 */
/** @brief Accepts the alphabet option without checking or changing its spelling. */
void
accept_alphabet_option(const char *value)
{
	(void) value;
}

} // namespace

bytea *
parse_relation_options(Datum options, bool validate)
{
	return static_cast<bytea *>(
		build_reloptions(options, validate, relation_option_kind,
						 sizeof(relation_options), relation_option_fields,
						 lengthof(relation_option_fields)));
}

namespace {

using external_relation_options_codec =
	bioseq::postgres::storage::metadata::sql_row_codecs::
		external_relation_options_codec;
using external_relation_options_row =
	external_relation_options_codec::row_type;

/** @return True when a CREATE TABLE option belongs to this access method. */
[[nodiscard]] bool
is_private_relation_option(const DefElem *option) noexcept
{
	if (option == nullptr || option->defnamespace != nullptr
		|| option->defname == nullptr)
	{
		return false;
	}
	return std::strcmp(option->defname, "entity") == 0
		|| std::strcmp(option->defname, "alphabet") == 0
		|| std::strcmp(option->defname, "storage_width") == 0
		|| std::strcmp(option->defname, "chunk_bases") == 0;
}

/** @return The options catalog OID, or InvalidOid if it is missing and missing_ok is true. */
[[nodiscard]] Oid
relation_options_catalog_oid(bool missing_ok)
{
	const Oid namespace_oid = get_namespace_oid("bioseq_internal", true);
	if (OidIsValid(namespace_oid))
	{
		const Oid relation_oid = get_relname_relid(
			"external_relation_options", namespace_oid);
		if (OidIsValid(relation_oid))
		{
			return relation_oid;
		}
	}
	if (missing_ok)
	{
		return InvalidOid;
	}
	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_TABLE),
			 errmsg("bioseq_internal.external_relation_options is missing")));
	pg_unreachable();
}

/** @return True when a PostgreSQL text datum equals the expected C string. */
[[nodiscard]] bool
text_equals(const text *value, const char *expected) noexcept
{
	const Size expected_size = std::strlen(expected);
	const Size actual_size = static_cast<Size>(VARSIZE_ANY_EXHDR(value));
	return actual_size == expected_size
		&& std::memcmp(VARDATA_ANY(value), expected, actual_size) == 0;
}

/** @return The parsed entity or `unspecified` for an unknown spelling. */
[[nodiscard]] relation_entity
entity_from_text(const text *value) noexcept
{
	constexpr relation_entity candidates[] = {
		relation_entity::taxonomy,
		relation_entity::catalog,
		relation_entity::record_gene,
		relation_entity::record_location,
		relation_entity::sequence,
		relation_entity::sequence_chunk,
	};
	for (const relation_entity candidate : candidates)
	{
		if (text_equals(value, to_string(candidate)))
		{
			return candidate;
		}
	}
	return relation_entity::unspecified;
}

} // namespace

bool
is_external_create_statement(const CreateStmt *statement) noexcept
{
	return statement != nullptr && statement->accessMethod != nullptr
		&& std::strcmp(statement->accessMethod, "bioseq_external") == 0;
}

create_relation_options
consume_create_relation_options(CreateStmt *statement)
{
	create_relation_options result{
		.applies = is_external_create_statement(statement),
		.entity = relation_entity::unspecified,
		.alphabet = nullptr,
		.storage_width = 0,
		.chunk_bases = 0,
	};
	if (!result.applies)
	{
		return result;
	}
	if (relation_option_kind == static_cast<relopt_kind>(0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("bioseq_external relation options are not initialized")));
	}

	List *private_options = NIL;
	List *postgres_options = NIL;
	ListCell *cell = nullptr;
	foreach(cell, statement->options)
	{
		auto *option = reinterpret_cast<DefElem *>(lfirst(cell));
		if (is_private_relation_option(option))
		{
			private_options = lappend(private_options, option);
		}
		else
		{
			postgres_options = lappend(postgres_options, option);
		}
	}

	if (private_options == NIL)
	{
		list_free(postgres_options);
		return result;
	}

	List *original_options = statement->options;
	statement->options = postgres_options;
	list_free(original_options);

	char *valid_namespaces[] = { nullptr };
	const Datum serialized = transformRelOptions(
		(Datum) 0, private_options, nullptr, valid_namespaces,
		/* acceptOidsOff = */ false, /* isReset = */ false);
	list_free(private_options);

	auto *parsed = reinterpret_cast<relation_options *>(
		parse_relation_options(serialized, true));
	if (parsed == nullptr
		|| VARSIZE_ANY(parsed) < static_cast<int>(sizeof(relation_options)))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid bioseq_external relation options")));
	}

	result.entity = static_cast<relation_entity>(parsed->entity);
	result.alphabet = GET_STRING_RELOPTION(parsed, alphabet);
	result.storage_width = parsed->storage_width;
	result.chunk_bases = parsed->chunk_bases;
	return result;
}

void
persist_create_relation_options(
	Oid relation_oid, const create_relation_options& options)
{
	if (!options.applies)
	{
		return;
	}

	Relation target = table_open(relation_oid, AccessShareLock);
	const bool external_relation = uses_table_access_method(target);
	table_close(target, AccessShareLock);
	if (!external_relation)
	{
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("created relation does not use the bioseq_external access method")));
	}

	std::optional<text *> stored_entity;
	if (options.entity != relation_entity::unspecified)
	{
		stored_entity = cstring_to_text(to_string(options.entity));
	}
	std::optional<text *> stored_alphabet;
	if (options.alphabet != nullptr)
	{
		stored_alphabet = cstring_to_text(options.alphabet);
	}

	const Oid catalog_oid = relation_options_catalog_oid(false);
	Relation catalog = table_open(catalog_oid, RowExclusiveLock);
	TupleDesc descriptor = RelationGetDescr(catalog);
	external_relation_options_codec::validate_descriptor(descriptor);

	auto encoded = external_relation_options_codec::encode(
		external_relation_options_row{
			.regclass = relation_oid,
			.entity = stored_entity,
			.alphabet = stored_alphabet,
			.storage_width = options.storage_width,
			.chunk_bases = options.chunk_bases,
		},
		CurrentMemoryContext);
	HeapTuple replacement = encoded.form_tuple(descriptor);

	ScanKeyData key = external_relation_options_codec::scan_key<
		&external_relation_options_row::regclass>(relation_oid);
	SysScanDesc scan = systable_beginscan(
		catalog, InvalidOid, false, SnapshotSelf, 1, &key);
	HeapTuple existing = systable_getnext(scan);
	if (HeapTupleIsValid(existing))
	{
		CatalogTupleUpdate(catalog, &existing->t_self, replacement);
	}
	else
	{
		CatalogTupleInsert(catalog, replacement);
	}
	systable_endscan(scan);
	heap_freetuple(replacement);
	table_close(catalog, RowExclusiveLock);
	CommandCounterIncrement();
}

bool
delete_relation_options(Oid relation_oid, bool missing_ok)
{
	const Oid catalog_oid = relation_options_catalog_oid(true);
	if (!OidIsValid(catalog_oid))
	{
		return false;
	}

	Relation catalog = table_open(catalog_oid, RowExclusiveLock);
	external_relation_options_codec::validate_descriptor(
		RelationGetDescr(catalog));
	ScanKeyData key = external_relation_options_codec::scan_key<
		&external_relation_options_row::regclass>(relation_oid);
	SysScanDesc scan = systable_beginscan(
		catalog, InvalidOid, false, SnapshotSelf, 1, &key);
	HeapTuple existing = systable_getnext(scan);
	const bool removed = HeapTupleIsValid(existing);
	if (removed)
	{
		CatalogTupleDelete(catalog, &existing->t_self);
	}
	systable_endscan(scan);
	table_close(catalog, RowExclusiveLock);
	if (removed)
	{
		CommandCounterIncrement();
	}
	else if (!missing_ok)
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("bioseq_external relation option metadata is missing for relation %u",
						relation_oid)));
	}
	return removed;
}

relation_entity
entity_from_options(Relation relation)
{
	if (relation == nullptr || !uses_table_access_method(relation))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation does not use the bioseq_external access method")));
	}

	const Oid catalog_oid = relation_options_catalog_oid(false);
	relation_entity entity = relation_entity::unspecified;
	bool row_found = false;
	bool entity_present = false;
	Relation catalog = table_open(catalog_oid, AccessShareLock);
	TupleDesc descriptor = RelationGetDescr(catalog);
	external_relation_options_codec::validate_descriptor(descriptor);
	ScanKeyData key = external_relation_options_codec::scan_key<
		&external_relation_options_row::regclass>(
			RelationGetRelid(relation));
	SysScanDesc scan = systable_beginscan(
		catalog, InvalidOid, false, SnapshotSelf, 1, &key);
	HeapTuple tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
	{
		row_found = true;
		external_relation_options_row stored =
			external_relation_options_codec::deform(
				tuple, descriptor, CurrentMemoryContext);
		if (stored.entity.has_value())
		{
			entity_present = true;
			entity = entity_from_text(*stored.entity);
		}
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);

	if (!row_found || !entity_present)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("bioseq_external relation \"%s\" is missing the required \"entity\" option",
						RelationGetRelationName(relation)),
				 errhint("Valid entity values are taxonomy, catalog, record_gene, "
						 "record_location, sequence, and sequence_chunk.")));
	}

	switch (entity)
	{
		case relation_entity::taxonomy:
		case relation_entity::catalog:
		case relation_entity::record_gene:
		case relation_entity::record_location:
		case relation_entity::sequence:
		case relation_entity::sequence_chunk:
			return entity;
		case relation_entity::unspecified:
			break;
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("invalid \"entity\" option for bioseq_external relation \"%s\"",
					RelationGetRelationName(relation))));
	pg_unreachable();
}

/* ======================================================================== */
/* Error helpers                                                             */
/* ======================================================================== */

namespace {

/** @brief Reports that external rows must be read through BioseqExternalScan. */
[[noreturn]] void
raise_custom_scan_required()
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("Custom Scan required"),
			 errdetail("bioseq_external relations are read through the bioseq_external "
					   "Custom Scan; a physical scan path was reached instead."),
			 errhint("Ensure the extension is loaded and "
			         "bioseq.planner.custom_scan.enabled is on.")));
	pg_unreachable();
}

/** @brief Rejects writes against an external read-only relation. */
[[noreturn]] void
raise_read_only()
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("external relations are read-only")));
	pg_unreachable();
}

/** @brief Reports an operation that external tables do not support. */
[[noreturn]] void
raise_unsupported_operation(const char *feature)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("%s is not supported on bioseq_external relations", feature)));
	pg_unreachable();
}

} // namespace

/* ======================================================================== */
/* TableAmRoutine callbacks for the supported PostgreSQL versions. */
/* Version checks below select the signatures required by PostgreSQL. */
/* ======================================================================== */

namespace {

/* Virtual tuple slots for rows returned by the custom scan */

/** @return Virtual slot operations used by provider-produced rows. */
const TupleTableSlotOps *
slot_callbacks(Relation relation)
{
	return &TTSOpsVirtual;
}

/* Sequential reads go through the custom scan. */

/** @brief Rejects a physical  scan because external rows require the custom scan. */
TableScanDesc
begin_physical_scan(Relation relation, Snapshot snapshot,
						   int nkeys, ScanKey key,
						   ParallelTableScanDesc parallel_scan,
						   uint32 flags)
{
	raise_custom_scan_required();
}

/** @brief Does nothing because physical scan creation always fails. */
void
end_physical_scan(TableScanDesc scan)
{
	/* begin_physical_scan never returns a descriptor to free. */
}

/** @brief Rejects rescan through the physical table callback. */
void
rescan_physical_scan(TableScanDesc scan, ScanKey key, bool set_params,
					   bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	raise_custom_scan_required();
}

/** @brief Rejects tuple retrieval through the physical table callback. */
bool
next_physical_tuple(TableScanDesc scan, ScanDirection direction,
							TupleTableSlot *slot)
{
	raise_custom_scan_required();
}

/* External scans do not support PostgreSQL parallel workers. */

/** @brief Rejects parallel-scan size estimation for external relations. */
Size
estimate_parallel_scan(Relation rel)
{
	raise_unsupported_operation("parallel scan");
}

/** @brief Rejects parallel-scan initialization for external relations. */
Size
initialize_parallel_scan(Relation rel, ParallelTableScanDesc pscan)
{
	raise_unsupported_operation("parallel scan");
}

/** @brief Rejects parallel-scan reinitialization for external relations. */
void
reinitialize_parallel_scan(Relation rel, ParallelTableScanDesc pscan)
{
	raise_unsupported_operation("parallel scan");
}

/* External tables have no local indexes. */

#if PG_VERSION_NUM >= 200000
/** Rejects PG20 index-scan creation, as the external AM has no local indexes. */
void begin_index_scan(IndexScanDesc scan, uint32 flags)
{
    raise_unsupported_operation("index scan");
}

/** Accepts cleanup after the index scan's creation was rejected. */
void reset_index_scan(IndexScanDesc scan) {}

/** Accepts final cleanup after the index scan's creation was rejected. */
void end_index_scan(IndexScanDesc scan) {}

/** Rejects constraint checks that look up rows by a physical tuple ID. */
bool fetch_visible_tid(Relation relation, ItemPointer tid,
                       Snapshot snapshot, bool *all_dead)
{
    raise_unsupported_operation("index scan");
}
#else
/** @brief Rejects index fetch creation for external relations. */
IndexFetchTableData *
begin_index_fetch(Relation rel
#if PG_VERSION_NUM >= 190000
                  , uint32 flags
#endif
)
{
	raise_unsupported_operation("index scan");
}

/** @brief Accepts cleanup after index fetch creation was rejected. */
void
reset_index_fetch(IndexFetchTableData *scan)
{
	/* begin_index_fetch never returns a handle to resed. */
}

/** @brief Accepts final cleanup after index fetch creation was rejected. */
void
end_index_fetch(IndexFetchTableData *scan)
{
	/* begin_index_fetch never returns a handle to free. */
}

/** @brief Rejects tuple retrieval through a local index. */
bool
fetch_index_tuple(IndexFetchTableData *scan, ItemPointer tid,
								  Snapshot snapshot, TupleTableSlot *slot,
								  bool *call_again, bool *all_dead)
{
	raise_unsupported_operation("index scan");
}
#endif

/* Physical row identifiers and snapshot checks. */

/** @brief Rejects physical tuple-version retrieval. */
bool
fetch_row_version(Relation relation, ItemPointer tid,
								  Snapshot snapshot, TupleTableSlot *slot)
{
	raise_custom_scan_required();
}

/** @return False because external relations expose no physical tuple IDs. */
bool
tuple_identifier_is_valid(TableScanDesc scan, ItemPointer tid)
{
	/* A physical tuple ID cannot identify an external row. */
	return false;
}

/** @brief Rejects physical tuple-version chain traversal. */
void
get_latest_tuple_identifier(TableScanDesc scan, ItemPointer tid)
{
	raise_unsupported_operation("tuple version chains");
}

/** @return False because external relations store no local tuple versions. */
bool
tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
										 Snapshot snapshot)
{
	/* No stored tuple can satisfy a snapshot. */
	return false;
}

/** @return InvalidTransactionId, with the number of index entries to delete  set to zero. */
TransactionId
delete_index_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	/* External tables have no local index entries to delete. */
	delstate->ndeltids = 0;
	return InvalidTransactionId;
}

/* External tables are read-only. */

/** @brief Rejects insertion into a read-only external relation. */
void
insert_tuple(Relation relation, TupleTableSlot *slot,
			 CommandId cid,
#if PG_VERSION_NUM >= 190000
             uint32 options,
#else
             int options,
#endif
             BulkInsertState bistate)
{
	raise_read_only();
}

/** @brief Rejects speculative insertion into a read-only external relation. */
void
insert_speculative_tuple(Relation relation, TupleTableSlot *slot,
										 CommandId cid,
#if PG_VERSION_NUM >= 190000
                                         uint32 options,
#else
                                         int options,
#endif
										 BulkInsertState bistate, uint32 specToken)
{
	raise_read_only();
}

/** @brief Rejects speculative-insert completion for an external relation. */
void
complete_speculative_tuple(Relation relation, TupleTableSlot *slot,
										   uint32 specToken, bool succeeded)
{
	raise_read_only();
}

/** @brief Rejects batch insertion into a read-only external relation. */
void
insert_multiple_tuples(Relation relation, TupleTableSlot **slots, int ntuples,
                       CommandId cid,
#if PG_VERSION_NUM >= 190000
                       uint32 options,
#else
                       int options,
#endif
                       BulkInsertState bistate)
{
	raise_read_only();
}

/** @brief Rejects tuple deletion from a read-only external relation. */
TM_Result
delete_tuple(Relation relation, ItemPointer tid, CommandId cid,
#if PG_VERSION_NUM >= 190000
             uint32 options,
#endif
							 Snapshot snapshot, Snapshot crosscheck, bool wait,
                             TM_FailureData *tmfd
#if PG_VERSION_NUM < 190000
                             , bool changingPart
#endif
)
{
	raise_read_only();
}

/** @brief Rejects tuple updates on a read-only external relation. */
TM_Result
update_tuple(Relation relation, ItemPointer otid, TupleTableSlot *slot,
                             CommandId cid,
#if PG_VERSION_NUM >= 190000
                             uint32 options,
#endif
                             Snapshot snapshot, Snapshot crosscheck,
							 bool wait, TM_FailureData *tmfd,
							 LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	raise_read_only();
}

/** @brief Rejects physical tuple locks on an external relation. */
TM_Result
lock_tuple(Relation relation, ItemPointer tid, Snapshot snapshot,
						   TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
						   LockWaitPolicy wait_policy, uint8 flags,
						   TM_FailureData *tmfd)
{
	raise_read_only();
}

/** @brief Rejects bulk-insert finalization for an external relation. */
void
finish_bulk_insert(Relation relation,
#if PG_VERSION_NUM >= 190000
                   uint32 options
#else
                   int options
#endif
)
{
	raise_read_only();
}

/* External tables do not create local storage files. */

/** @brief Rejects temporary tables and marks transaction freeze limits as unused. */
void
set_new_file_locator(Relation rel,
											 const RelFileLocator *newrlocator,
											 char persistence,
											 TransactionId *freezeXid,
											 MultiXactId *minmulti)
{
	if (persistence == RELPERSISTENCE_TEMP)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("bioseq_external relations cannot be TEMPORARY")));
	}

	/*
	 * The ProcessUtility hook removes these options before CREATE TABLE and
	 * saves them after it succeeds. This table is not yet visible in the
	 * catalog, so entity_from_options checks the required entity at scan time.
	 */

	/*
	 * External tables have no local rows or storage files. Their size is zero,
	 * and, transaction freezing has nothing to  track.
	 */
	*freezeXid = InvalidTransactionId;
	*minmulti = InvalidMultiXactId;
}

/** @brief Rejects nontransactional truncation of an external relation. */
void
truncate_nontransactional(Relation rel)
{
	raise_unsupported_operation("TRUNCATE");
}

/** @brief Rejects physical data copying for an external relation. */
void
copy_relation_data(Relation rel, const RelFileLocator *newrlocator)
{
	raise_unsupported_operation("storage rewrite");
}

/** @brief Rejects physical relation rewrites for an external relation. */
void
copy_relation_for_cluster(Relation OldTable, Relation NewTable,
										  Relation OldIndex, bool use_sort,
										  TransactionId OldestXmin,
#if PG_VERSION_NUM >= 190000
                                          Snapshot snapshot,
#endif
										  TransactionId *xid_cutoff,
										  MultiXactId *multi_cutoff,
										  double *num_tuples,
										  double *tups_vacuumed,
										  double *tups_recently_dead)
{
	raise_unsupported_operation("CLUSTER / VACUUM FULL");
}

/** @brief Does nothing because external tables have no local pages to vacuum */
void
vacuum_relation(Relation rel,
#if PG_VERSION_NUM >= 190000
                const VacuumParams *params,
#else
                VacuumParams *params,
#endif
								BufferAccessStrategy bstrategy)
{
	/* VACUUM and autovacuum have no local pages to clhan. */
}

/* ANALYZE has no local rows to sample and makes no provider requests. */

/** @return False because external relations expose no local blocks to analyze. */
bool
next_analyze_block(TableScanDesc scan, ReadStream *stream)
{
	/* No physical blocks to sample. */
	return false;
}

/** @return False without contacting a provider or yielding a sample tuple. */
bool
next_analyze_tuple(TableScanDesc scan,
#if PG_VERSION_NUM < 190000
                  TransactionId OldestXmin,
#endif
										double *liverows, double *deadrows,
										TupleTableSlot *slot)
{
	/* Return no sample rows and make no provider request. */
	return false;
}

/* Local index creation and validation are unsupported. */

/** @brief Rejects local index construction for external relations. */
double
build_index_range_scan(Relation table_rel, Relation index_rel,
									   IndexInfo *index_info, bool allow_sync,
									   bool anyvisible, bool progress,
									   BlockNumber start_blockno,
									   BlockNumber numblocks,
									   IndexBuildCallback callback,
									   void *callback_state, TableScanDesc scan)
{
	raise_unsupported_operation("index build");
}

/** @brief Rejects local index validation for external relations. */
void
validate_index_scan(Relation table_rel, Relation index_rel,
									IndexInfo *index_info, Snapshot snapshot,
									ValidateIndexState *state)
{
	raise_unsupported_operation("index validate");
}

/* Local storage size and planner estimates. */

/** @return Zero because external tables have no local storage files. */
uint64
relation_size(Relation rel, ForkNumber forkNumber)
{
	/* No physical storage. */
	return 0;
}

/** @return False because no local tuples require TOAST storage. */
bool
relation_needs_toast_table(Relation rel)
{
	/* No tuples are stored, so no TOAST table is ever needed. */
	return false;
}

/* Default row estimate used without contacting the provider. */
constexpr double k_default_estimated_rows = 1000.0;

/** @brief Estimates table size  from a fixed row count and local column widths. */
void
estimate_relation_size(Relation rel, int32 *attr_widths,
								  BlockNumber *pages, double *tuples,
								  double *allvisfrac)
{
	*pages = 0;
	*tuples = k_default_estimated_rows;
	*allvisfrac = 0.0;

	/* Read column widths from local statistics or types. */
	get_rel_data_width(rel, attr_widths);
}

/* TABLESAMPLE is unsupported. */

/** @brief Rejects block sampling for external relations. */
bool
next_sample_block(TableScanDesc scan, SampleScanState *scanstate)
{
	raise_unsupported_operation("TABLESAMPLE");
}

/** @brief Rejects tuple sampling for external relations. */
bool
next_sample_tuple(TableScanDesc scan, SampleScanState *scanstate,
									   TupleTableSlot *slot)
{
	raise_unsupported_operation("TABLESAMPLE");
}

/** @brief External table callbacks, with bitmap scanning disabled. */
const TableAmRoutine table_access_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = slot_callbacks,

	.scan_begin = begin_physical_scan,
	.scan_end = end_physical_scan,
	.scan_rescan = rescan_physical_scan,
	.scan_getnextslot = next_physical_tuple,

	.parallelscan_estimate = estimate_parallel_scan,
	.parallelscan_initialize = initialize_parallel_scan,
	.parallelscan_reinitialize = reinitialize_parallel_scan,

#if PG_VERSION_NUM >= 200000
    .index_scan_begin = begin_index_scan,
    .index_scan_reset = reset_index_scan,
    .index_scan_end = end_index_scan,
    .fetch_tid = fetch_visible_tid,
#else
	.index_fetch_begin = begin_index_fetch,
	.index_fetch_reset = reset_index_fetch,
	.index_fetch_end = end_index_fetch,
	.index_fetch_tuple = fetch_index_tuple,
#endif

	.tuple_fetch_row_version = fetch_row_version,
	.tuple_tid_valid = tuple_identifier_is_valid,
	.tuple_get_latest_tid = get_latest_tuple_identifier,
	.tuple_satisfies_snapshot = tuple_satisfies_snapshot,
	.index_delete_tuples = delete_index_tuples,

	.tuple_insert = insert_tuple,
	.tuple_insert_speculative = insert_speculative_tuple,
	.tuple_complete_speculative = complete_speculative_tuple,
	.multi_insert = insert_multiple_tuples,
	.tuple_delete = delete_tuple,
	.tuple_update = update_tuple,
	.tuple_lock = lock_tuple,
	.finish_bulk_insert = finish_bulk_insert,

	.relation_set_new_filelocator = set_new_file_locator,
	.relation_nontransactional_truncate = truncate_nontransactional,
	.relation_copy_data = copy_relation_data,
	.relation_copy_for_cluster = copy_relation_for_cluster,
	.relation_vacuum = vacuum_relation,
	.scan_analyze_next_block = next_analyze_block,
	.scan_analyze_next_tuple = next_analyze_tuple,
	.index_build_range_scan = build_index_range_scan,
	.index_validate_scan = validate_index_scan,

	.relation_size = relation_size,
	.relation_needs_toast_table = relation_needs_toast_table,

	.relation_estimate_size = estimate_relation_size,

/* PostgreSQL 18 removed the per-block bitmap callback. */
#if PG_VERSION_NUM < PG_VERSION_18
	.scan_bitmap_next_block = NULL,
#endif
	.scan_bitmap_next_tuple = NULL,
	.scan_sample_next_block = next_sample_block,
	.scan_sample_next_tuple = next_sample_tuple
};

} // namespace

const TableAmRoutine *
table_access_method() noexcept
{
	return &table_access_methods;
}

bool
uses_table_access_method(Relation relation) noexcept
{
	return relation != nullptr && relation->rd_tableam == &table_access_methods;
}

const char *
to_string(relation_entity entity) noexcept
{
	switch (entity)
	{
		case relation_entity::taxonomy:
			return "taxonomy";
		case relation_entity::catalog:
			return "catalog";
		case relation_entity::record_gene:
			return "record_gene";
		case relation_entity::record_location:
			return "record_location";
		case relation_entity::sequence:
			return "sequence";
		case relation_entity::sequence_chunk:
			return "sequence_chunk";
		case relation_entity::unspecified:
			return "unspecified";
	}
	return "unspecified";
}

/** @brief Registers relation options once for the backend process. */
void
initialize_relation_options()
{
	if (relation_option_kind != static_cast<relopt_kind>(0))
	{
		return;
	}

	relation_option_kind = add_reloption_kind();

	add_enum_reloption(relation_option_kind, "entity",
					   "Logical entity kind for a bioseq_external relation.",
					   entity_option_values,
					   static_cast<int>(relation_entity::unspecified),
					   "Valid entity values are \"taxonomy\", \"catalog\", "
					   "\"record_gene\", \"record_location\", \"sequence\", "
					   "and \"sequence_chunk\".",
					   AccessExclusiveLock);

	add_string_reloption(relation_option_kind, "alphabet",
						 "Sequence alphabet for sequence/sequence_chunk entities.",
						 nullptr, accept_alphabet_option,
						 AccessExclusiveLock);

	add_int_reloption(relation_option_kind, "storage_width",
					  "Sequence storage unit width for sequence entities.",
					  0, 0, 64, AccessExclusiveLock);

	add_int_reloption(relation_option_kind, "chunk_bases",
					  "Chunk length in bases for sequence_chunk entities.",
					  0, 0, std::numeric_limits<int>::max(), AccessExclusiveLock);
}

} // namespace bioseq::postgres::external

extern "C" void
bioseq_external_tableam_init(void)
{
	bioseq::postgres::external::initialize_relation_options();
}

namespace bioseq::postgres::external::sql_routines {

POSTGRES_FN(bioseq_external_handler)
{
	PG_RETURN_POINTER(bioseq::postgres::external::table_access_method());
}

} // namespace bioseq::postgres::external::sql_routines
