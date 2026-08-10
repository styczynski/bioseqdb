#pragma once

#define PG_VERSION_17 170000
#define PG_VERSION_18 180000

#ifdef __cplusplus
/* PostgreSQL uses this compiler builtin in C-only declaration helpers. */
#undef HAVE__BUILTIN_TYPES_COMPATIBLE_P
#define __builtin_types_compatible_p(a, b) (1)
#endif

/* C++-safe equivalent of PG_FUNCTION_INFO_V1 plus the function declaration. */
#define POSTGRES_FN(funcname) \
extern "C" PGDLLEXPORT const Pg_finfo_record * \
CppConcat(pg_finfo_,funcname) (void) \
{ \
	static const Pg_finfo_record my_finfo = { 1 }; \
	return &my_finfo; \
} \
extern "C" int no_such_variable; \
extern "C" PGDLLEXPORT Datum funcname(PG_FUNCTION_ARGS)

extern "C" {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wregister"

#include "c.h"
#include "postgres.h"

#if PG_VERSION_NUM < PG_VERSION_17
#error "Unsupported Postgres version lower than 17"
#endif

#include "storage/relfilelocator.h"
#include "utils/array.h"
#include "optimizer/tlist.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "nodes/pg_list.h"
#include "utils/array.h"
#include "utils/hsearch.h"
#include "nodes/makefuncs.h"
#include "nodes/nodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "utils/rel.h"
#include "fmgr.h"
#include "access/tableam.h"
#include "access/skey.h"
#include "nodes/bitmapset.h"
#include "access/heapam.h"
#include "catalog/indexing.h"
#include "utils/acl.h"
#include "nodes/extensible.h"
#include "nodes/print.h"
#include "utils/palloc.h"
#include "nodes/pg_list.h"
#include "commands/tablecmds.h"
#include "catalog/pg_attribute.h"
#include "utils/syscache.h"
#include "catalog/pg_statistic_ext.h"
#include "access/htup_details.h"
#include "catalog/pg_statistic_ext.h"
#include "access/relation.h"
#include "nodes/execnodes.h"
#include "executor/tuptable.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "nodes/parsenodes.h"
#include "nodes/pathnodes.h"
#include "nodes/extensible.h"
#include "port/atomics.h"
#include "storage/bufpage.h"
#include "storage/lockdefs.h"
#include "storage/relfilelocator.h"
#include "storage/s_lock.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "fmgr.h"
#include "access/tableam.h"
#include "access/skey.h"
#include "nodes/bitmapset.h"
#include "access/heapam.h"
#include "catalog/indexing.h"
#include "utils/acl.h"
#include "access/sdir.h"
#include "storage/smgr.h"
#include "utils/rel.h"
#include "nodes/plannodes.h"
#include "utils/lsyscache.h"
#include "varatt.h"
#include "nodes/bitmapset.h"
#include "nodes/primnodes.h"
#include "utils/hsearch.h"
#include "funcapi.h"
#include "access/nbtree.h"
#include "access/table.h"
#include "catalog/pg_am.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "catalog/namespace.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "lib/stringinfo.h"
#include "parser/parse_relation.h"
#include "port.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "storage/relfilelocator.h"
#include "utils/relfilenumbermap.h"
#include "access/nbtree.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "catalog/pg_am.h"
#include "catalog/index.h"
#include "executor/execdebug.h"
#include "executor/nodeIndexscan.h"
#include "lib/pairingheap.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/nodeFuncs.h"
#include "storage/predicate.h"
#include "utils/array.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "catalog/pg_type.h"
#include "utils/ruleutils.h"
#include "nodes/makefuncs.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/smgr.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"
#include "utils/numeric.h"
#include "access/amapi.h"
#include "access/skey.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_statistic.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/plancat.h"
#include "optimizer/restrictinfo.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/ruleutils.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/ilist.h"
#include "lib/stringinfo.h"
#include "utils/memutils.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "nodes/pg_list.h"
#include "nodes/makefuncs.h"
#include "parser/parse_oper.h"
#include "parser/parse_func.h"
#include "utils/syscache.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "common/fe_memutils.h"
#include "executor/execExpr.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "lib/hyperloglog.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_agg.h"
#include "parser/parse_coerce.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/dynahash.h"
#include "utils/expandeddatum.h"
#include "utils/logtape.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"
#include "utils/fmgrprotos.h"
#include "common/int.h"
#include "utils/relfilenumbermap.h"
#include "access/generic_xlog.h"
#include "catalog/storage.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "access/heaptoast.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "access/amapi.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "commands/defrem.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "tcop/utility.h"
#include "parser/parse_oper.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/spccache.h"
#include "access/nbtree.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "commands/defrem.h"
#include "storage/smgr.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/clauses.h"
#include "optimizer/restrictinfo.h"
#include "storage/fd.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/palloc.h"
#include "utils/rel.h"
#include "miscadmin.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "access/rewriteheap.h"
#include "access/tableam.h"
#include "access/tsmapi.h"
#include "storage/lockdefs.h"
#include "utils/palloc.h"
#include "utils/snapmgr.h"
#include "access/detoast.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_am.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_extension.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "commands/extension.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "nodes/pg_list.h"
#include "optimizer/plancat.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "access/heapam.h"
#include "access/nbtree.h"
#include "catalog/pg_am.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/smgr.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/rel.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "access/rewriteheap.h"
#include "access/tsmapi.h"
#include "access/heaptoast.h"
#include "common/hashfn.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_am.h"
#include "catalog/pg_trigger.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "optimizer/plancat.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "fmgr.h"
#include "nodes/execnodes.h"
#include "utils/date.h"
#include "utils/array.h"
#include "utils/numeric.h"
#include "utils/fmgrprotos.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/pg_rusage.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "access/heaptoast.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "common/pg_lzcompress.h"
#include "nodes/supportnodes.h"

/*
 * In PG18, ExplainProperty{Text,Integer,UInteger,Float,Bool,List,...} were
 * moved out of commands/explain.h into a dedicated commands/explain_format.h
 * header. Pull it in unconditionally on PG18 so call sites compile without
 * needing to add the new include locally.
 */
#if PG_VERSION_NUM >= PG_VERSION_18
#include "commands/explain_format.h"
#endif


#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_init_privs.h"
#include "catalog/pg_language.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_parameter_acl.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_publication_namespace.h"
#include "catalog/pg_publication_rel.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_transform.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"

/*
 * PostgreSQL 17/18 boundary adapters.
 *
 * Each name states the Bioseq behavior selected at the boundary. These are
 * not aliases for superseded PostgreSQL spellings.
 */

/*
 * PG18 renamed BuildTupleHashTableExt and added the input slot operations.
 * Bioseq aggregate hashing feeds MinimalTuple-shaped slots.
 */
#if PG_VERSION_NUM >= PG_VERSION_18
#define bioseq_build_tuple_hash_table(parent, input_desc, column_count, \
									  key_column_indexes, equality_function_oids, \
									  hash_functions, collations, bucket_count, \
									  additional_size, metadata_context, \
									  table_context, temporary_context, \
									  use_variable_hash_iv) \
	BuildTupleHashTable((parent), (input_desc), &TTSOpsMinimalTuple, \
						(column_count), (key_column_indexes), \
						(equality_function_oids), (hash_functions), \
						(collations), (bucket_count), (additional_size), \
						(metadata_context), (table_context), \
						(temporary_context), (use_variable_hash_iv))
#else
#define bioseq_build_tuple_hash_table(parent, input_desc, column_count, \
									  key_column_indexes, equality_function_oids, \
									  hash_functions, collations, bucket_count, \
									  additional_size, metadata_context, \
									  table_context, temporary_context, \
									  use_variable_hash_iv) \
	BuildTupleHashTableExt((parent), (input_desc), (column_count), \
						   (key_column_indexes), (equality_function_oids), \
						   (hash_functions), (collations), (bucket_count), \
						   (additional_size), (metadata_context), \
						   (table_context), (temporary_context), \
						   (use_variable_hash_iv))
#endif

/*
 * PG18 added runtime-pruning state to ExecInitRangeTable. Bioseq metadata
 * writes have no pruning information.
 */
#if PG_VERSION_NUM >= PG_VERSION_18
#define bioseq_exec_init_range_table_without_pruning(estate, range_table, \
													 permission_infos) \
	ExecInitRangeTable((estate), (range_table), (permission_infos), NULL)
#else
#define bioseq_exec_init_range_table_without_pruning(estate, range_table, \
													 permission_infos) \
	ExecInitRangeTable((estate), (range_table), (permission_infos))
#endif

/*
 * PG18 added optional index-scan instrumentation. These custom scans do not
 * request it.
 */
#if PG_VERSION_NUM >= PG_VERSION_18
#define bioseq_begin_uninstrumented_parallel_index_scan(heap_relation, \
														index_relation, key_count, \
														order_by_count, parallel_scan) \
	index_beginscan_parallel((heap_relation), (index_relation), NULL, \
							 (key_count), (order_by_count), (parallel_scan))
#else
#define bioseq_begin_uninstrumented_parallel_index_scan(heap_relation, \
														index_relation, key_count, \
														order_by_count, parallel_scan) \
	index_beginscan_parallel((heap_relation), (index_relation), (key_count), \
							 (order_by_count), (parallel_scan))
#endif

/*
 * PG18 expanded parallel-scan initialization with instrumentation and worker
 * bookkeeping. Bioseq initializes the descriptor without either facility.
 */
#if PG_VERSION_NUM >= PG_VERSION_18
#define bioseq_initialize_uninstrumented_parallel_index_scan(heap_relation, \
															 index_relation, snapshot, target) \
	index_parallelscan_initialize((heap_relation), (index_relation), (snapshot), \
								  false, false, 0, NULL, (target))
#else
#define bioseq_initialize_uninstrumented_parallel_index_scan(heap_relation, \
															 index_relation, snapshot, target) \
	index_parallelscan_initialize( \
		(heap_relation), (index_relation), (snapshot), (target))
#endif

/*
 * PG18 stores TupleHashEntry additional data inline and exposes it through a
 * table-aware accessor. PG17 stores a separately allocated pointer.
 */
#if PG_VERSION_NUM >= PG_VERSION_18
#define bioseq_tuple_hash_entry_additional(hash_table, entry) \
	TupleHashEntryGetAdditional((hash_table), (entry))
#define bioseq_prepare_tuple_hash_entry_additional(hash_table, entry, size) \
	TupleHashEntryGetAdditional((hash_table), (entry))
#else
#define bioseq_tuple_hash_entry_additional(hash_table, entry) ((entry)->additional)
#define bioseq_prepare_tuple_hash_entry_additional(hash_table, entry, size) \
	((entry)->additional = MemoryContextAlloc((hash_table)->tablecxt, (size)))
#endif

#pragma GCC diagnostic pop
}

// These Min/Max macros from Postgres pollute global namespace and collide
// with std::min / std::max; undef them so modern C++ code can use the
// standard algorithms without qualification.
#undef Min
#undef Max

// Postgres maps stdio names to pg_* variants on some platforms. Leaving those
// macros active in C++ translation units rewrites libc++ internals such as
// std::vsnprintf into std::pg_vsnprintf.
#undef snprintf
#undef vsnprintf
