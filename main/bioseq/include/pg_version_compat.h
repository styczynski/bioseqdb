/*-------------------------------------------------------------------------
 *
 * pg_version_compat.h
 *	  Compatibility macros for writing code agnostic to PostgreSQL versions
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_VERSION_COMPAT_H
#define PG_VERSION_COMPAT_H

#ifdef __cplusplus
#undef HAVE__BUILTIN_TYPES_COMPATIBLE_P
#define __builtin_types_compatible_p(a, b) (1)
#endif

// CXX support
#define CXX_PG_FUNCTION_INFO_V1(funcname) \
extern "C" PGDLLEXPORT Datum funcname(PG_FUNCTION_ARGS); \
extern "C" PGDLLEXPORT const Pg_finfo_record * CppConcat(pg_finfo_,funcname)(void); \
const Pg_finfo_record * \
CppConcat(pg_finfo_,funcname) (void) \
{ \
	static const Pg_finfo_record my_finfo = { 1 }; \
	return &my_finfo; \
} \
extern "C" int no_such_variable


extern "C" {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wregister"

#include "c.h"
#include "citus_version.h"
#include "postgres.h"
#include "pg_version_constants.h"
#include "storage/relfilelocator.h"
#include "utils/array.h"
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
#include "pg_version_constants.h"
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
#include "columnar/utils/listutils.h"
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


// OLD COMPAT


#define RelationPhysicalIdentifier_compat(a) ((a)->rd_locator)
#define RelationTablespace_compat(a) (a.spcOid)
#define RelationPhysicalIdentifierNumber_compat(a) (a.relNumber)
#define tuplesort_getdatum_compat(a, b, c, d, e, f) tuplesort_getdatum(a, b, c, d, e, f)
#define AlterTableStmtObjType_compat(a) ((a)->objtype)
#define getObjectTypeDescription_compat(a, b) getObjectTypeDescription(a, b)
#define getObjectIdentity_compat(a, b) getObjectIdentity(a, b)
/* for MemoryContextMethods->stats */
#define stats_compat(a, b, c, d, e) stats(a, b, c, d, e)
#define FuncnameGetCandidates_compat(a, b, c, d, e, f, g) \
	FuncnameGetCandidates(a, b, c, d, e, f, g)
#define expand_function_arguments_compat(a, b, c, d) expand_function_arguments(a, b, c, d)
#define BeginCopyFrom_compat(a, b, c, d, e, f, g, h) BeginCopyFrom(a, b, c, d, e, f, g, h)
#define standard_ProcessUtility_compat(a, b, c, d, e, f, g, h) \
	standard_ProcessUtility(a, b, c, d, e, f, g, h)
#define ProcessUtility_compat(a, b, c, d, e, f, g, h) \
	ProcessUtility(a, b, c, d, e, f, g, h)
#define SetTuplestoreDestReceiverParams_compat(a, b, c, d, e, f) \
	SetTuplestoreDestReceiverParams(a, b, c, d, e, f)
#define pgproc_statusflags_compat(pgproc) ((pgproc)->statusFlags)
#define get_partition_parent_compat(a, b) get_partition_parent(a, b)
#define RelationGetPartitionDesc_compat(a, b) RelationGetPartitionDesc(a, b)
#define make_simple_restrictinfo_compat(a, b) make_simple_restrictinfo(a, b)
#define pull_varnos_compat(a, b) pull_varnos(a, b)
#define lnext_compat(l, r) lnext(l, r)
#define list_delete_cell_compat(l, c, p) list_delete_cell(l, c)
#define pg_plan_query_compat(p, q, c, b) pg_plan_query(p, q, c, b)
#define planner_compat(p, c, b) planner(p, NULL, c, b)
#define standard_planner_compat(a, c, d) standard_planner(a, NULL, c, d)
#define GetSequencesOwnedByRelation(a) getOwnedSequences(a)
#define GetSequencesOwnedByColumn(a, b) getOwnedSequences_internal(a, b, 0)
#define CMDTAG_SELECT_COMPAT CMDTAG_SELECT
#define ExplainOnePlanCompat(a, b, c, d, e, f, g, h) \
	ExplainOnePlan(a, b, c, d, e, f, g, h)
#define SetListCellPtr(a, b) ((a)->ptr_value = (b))
#define RangeTableEntryFromNSItem(a) ((a)->p_rte)
#define QueryCompletionCompat QueryCompletion
#define CreateTableSlotForRel(rel) table_slot_create(rel, NULL)
#define MakeSingleTupleTableSlotCompat MakeSingleTupleTableSlot
#define AllocSetContextCreateExtended AllocSetContextCreateInternal
#define NextCopyFromCompat NextCopyFrom
#define ArrayRef SubscriptingRef
#define T_ArrayRef T_SubscriptingRef
#define or_clause is_orclause
#define GetSysCacheOid1Compat GetSysCacheOid1
#define GetSysCacheOid2Compat GetSysCacheOid2
#define GetSysCacheOid3Compat GetSysCacheOid3
#define GetSysCacheOid4Compat GetSysCacheOid4
#define fcGetArgValue(fc, n) ((fc)->args[n].value)
#define fcGetArgNull(fc, n) ((fc)->args[n].isnull)
#define fcSetArgExt(fc, n, val, is_null) \
	(((fc)->args[n].isnull = (is_null)), ((fc)->args[n].value = (val)))
#define fcSetArg(fc, n, value) fcSetArgExt(fc, n, value, false)
#define fcSetArgNull(fc, n) fcSetArgExt(fc, n, (Datum) 0, true)


// OLD COMAPT END

#if PG_VERSION_NUM >= PG_VERSION_18
#define create_foreignscan_path_compat(a, b, c, d, e, f, g, h, i, j, k) \
		create_foreignscan_path( \
			(a),        /* root            */ \
			(b),        /* rel             */ \
			(c),        /* target          */ \
			(d),        /* rows            */ \
			0,          /* disabled_nodes  */ \
			(e),        /* startup_cost    */ \
			(f),        /* total_cost      */ \
			(g),        /* pathkeys        */ \
			(h),        /* required_outer  */ \
			(i),        /* fdw_outerpath   */ \
			(j),        /* fdw_restrictinfo*/ \
			(k)         /* fdw_private     */ \
			)

/* PG-18 introduced get_op_index_interpretation, old name was get_op_btree_interpretation */
#define get_op_btree_interpretation(opno) get_op_index_interpretation(opno)

/* PG-18 unified row-compare operator codes under COMPARE_* */
#define ROWCOMPARE_NE COMPARE_NE

#elif PG_VERSION_NUM >= PG_VERSION_17
#define create_foreignscan_path_compat(a, b, c, d, e, f, g, h, i, j, k) \
		create_foreignscan_path( \
			(a), (b), (c), (d), \
			(e), (f), \
			(g), (h), (i), (j), (k) \
			)

#endif

#if PG_VERSION_NUM >= PG_VERSION_17

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
 * This enum covers all system catalogs whose OIDs can appear in
 * pg_depend.classId or pg_shdepend.classId.
 */
typedef enum ObjectClass
{
	OCLASS_CLASS,               /* pg_class */
	OCLASS_PROC,                /* pg_proc */
	OCLASS_TYPE,                /* pg_type */
	OCLASS_CAST,                /* pg_cast */
	OCLASS_COLLATION,           /* pg_collation */
	OCLASS_CONSTRAINT,          /* pg_constraint */
	OCLASS_CONVERSION,          /* pg_conversion */
	OCLASS_DEFAULT,             /* pg_attrdef */
	OCLASS_LANGUAGE,            /* pg_language */
	OCLASS_LARGEOBJECT,         /* pg_largeobject */
	OCLASS_OPERATOR,            /* pg_operator */
	OCLASS_OPCLASS,             /* pg_opclass */
	OCLASS_OPFAMILY,            /* pg_opfamily */
	OCLASS_AM,                  /* pg_am */
	OCLASS_AMOP,                /* pg_amop */
	OCLASS_AMPROC,              /* pg_amproc */
	OCLASS_REWRITE,             /* pg_rewrite */
	OCLASS_TRIGGER,             /* pg_trigger */
	OCLASS_SCHEMA,              /* pg_namespace */
	OCLASS_STATISTIC_EXT,       /* pg_statistic_ext */
	OCLASS_TSPARSER,            /* pg_ts_parser */
	OCLASS_TSDICT,              /* pg_ts_dict */
	OCLASS_TSTEMPLATE,          /* pg_ts_template */
	OCLASS_TSCONFIG,            /* pg_ts_config */
	OCLASS_ROLE,                /* pg_authid */
	OCLASS_ROLE_MEMBERSHIP,     /* pg_auth_members */
	OCLASS_DATABASE,            /* pg_database */
	OCLASS_TBLSPACE,            /* pg_tablespace */
	OCLASS_FDW,                 /* pg_foreign_data_wrapper */
	OCLASS_FOREIGN_SERVER,      /* pg_foreign_server */
	OCLASS_USER_MAPPING,        /* pg_user_mapping */
	OCLASS_DEFACL,              /* pg_default_acl */
	OCLASS_EXTENSION,           /* pg_extension */
	OCLASS_EVENT_TRIGGER,       /* pg_event_trigger */
	OCLASS_PARAMETER_ACL,       /* pg_parameter_acl */
	OCLASS_POLICY,              /* pg_policy */
	OCLASS_PUBLICATION,         /* pg_publication */
	OCLASS_PUBLICATION_NAMESPACE,   /* pg_publication_namespace */
	OCLASS_PUBLICATION_REL,     /* pg_publication_rel */
	OCLASS_SUBSCRIPTION,        /* pg_subscription */
	OCLASS_TRANSFORM,           /* pg_transform */
} ObjectClass;

#define LAST_OCLASS OCLASS_TRANSFORM

/*
 * Determine the class of a given object identified by objectAddress.
 *
 * We implement it as a function instead of an array because the OIDs aren't
 * consecutive.
 */
static inline ObjectClass
getObjectClass(const ObjectAddress *object)
{
	/* only pg_class entries can have nonzero objectSubId */
	if (object->classId != RelationRelationId &&
		object->objectSubId != 0)
	{
		elog(ERROR, "invalid non-zero objectSubId for object class %u",
			 object->classId);
	}

	switch (object->classId)
	{
		case RelationRelationId:
		{
			/* caller must check objectSubId */
			return OCLASS_CLASS;
		}

		case ProcedureRelationId:
		{
			return OCLASS_PROC;
		}

		case TypeRelationId:
		{
			return OCLASS_TYPE;
		}

		case CastRelationId:
		{
			return OCLASS_CAST;
		}

		case CollationRelationId:
		{
			return OCLASS_COLLATION;
		}

		case ConstraintRelationId:
		{
			return OCLASS_CONSTRAINT;
		}

		case ConversionRelationId:
		{
			return OCLASS_CONVERSION;
		}

		case AttrDefaultRelationId:
		{
			return OCLASS_DEFAULT;
		}

		case LanguageRelationId:
		{
			return OCLASS_LANGUAGE;
		}

		case LargeObjectRelationId:
		{
			return OCLASS_LARGEOBJECT;
		}

		case OperatorRelationId:
		{
			return OCLASS_OPERATOR;
		}

		case OperatorClassRelationId:
		{
			return OCLASS_OPCLASS;
		}

		case OperatorFamilyRelationId:
		{
			return OCLASS_OPFAMILY;
		}

		case AccessMethodRelationId:
		{
			return OCLASS_AM;
		}

		case AccessMethodOperatorRelationId:
		{
			return OCLASS_AMOP;
		}

		case AccessMethodProcedureRelationId:
		{
			return OCLASS_AMPROC;
		}

		case RewriteRelationId:
		{
			return OCLASS_REWRITE;
		}

		case TriggerRelationId:
		{
			return OCLASS_TRIGGER;
		}

		case NamespaceRelationId:
		{
			return OCLASS_SCHEMA;
		}

		case StatisticExtRelationId:
		{
			return OCLASS_STATISTIC_EXT;
		}

		case TSParserRelationId:
		{
			return OCLASS_TSPARSER;
		}

		case TSDictionaryRelationId:
		{
			return OCLASS_TSDICT;
		}

		case TSTemplateRelationId:
		{
			return OCLASS_TSTEMPLATE;
		}

		case TSConfigRelationId:
		{
			return OCLASS_TSCONFIG;
		}

		case AuthIdRelationId:
		{
			return OCLASS_ROLE;
		}

		case AuthMemRelationId:
		{
			return OCLASS_ROLE_MEMBERSHIP;
		}

		case DatabaseRelationId:
		{
			return OCLASS_DATABASE;
		}

		case TableSpaceRelationId:
		{
			return OCLASS_TBLSPACE;
		}

		case ForeignDataWrapperRelationId:
		{
			return OCLASS_FDW;
		}

		case ForeignServerRelationId:
		{
			return OCLASS_FOREIGN_SERVER;
		}

		case UserMappingRelationId:
		{
			return OCLASS_USER_MAPPING;
		}

		case DefaultAclRelationId:
		{
			return OCLASS_DEFACL;
		}

		case ExtensionRelationId:
		{
			return OCLASS_EXTENSION;
		}

		case EventTriggerRelationId:
		{
			return OCLASS_EVENT_TRIGGER;
		}

		case ParameterAclRelationId:
		{
			return OCLASS_PARAMETER_ACL;
		}

		case PolicyRelationId:
		{
			return OCLASS_POLICY;
		}

		case PublicationNamespaceRelationId:
		{
			return OCLASS_PUBLICATION_NAMESPACE;
		}

		case PublicationRelationId:
		{
			return OCLASS_PUBLICATION;
		}

		case PublicationRelRelationId:
		{
			return OCLASS_PUBLICATION_REL;
		}

		case SubscriptionRelationId:
		{
			return OCLASS_SUBSCRIPTION;
		}

		case TransformRelationId:
		{
			return OCLASS_TRANSFORM;
		}
	}

	/* shouldn't get here */
	elog(ERROR, "unrecognized object class: %u", object->classId);
	return OCLASS_CLASS;        /* keep compiler quiet */
}



static inline void
RangeVarCallbackOwnsTable(const RangeVar *relation,
						  Oid relId, Oid oldRelId, void *arg)
{
	return RangeVarCallbackMaintainsTable(relation, relId, oldRelId, arg);
}



static inline int
getAttstattarget_compat(HeapTuple attTuple)
{
	bool isnull;
	Datum dat = SysCacheGetAttr(ATTNUM, attTuple,
								Anum_pg_attribute_attstattarget, &isnull);
	return (isnull ? -1 : DatumGetInt16(dat));
}


static inline int
getStxstattarget_compat(HeapTuple tup)
{
	bool isnull;
	Datum dat = SysCacheGetAttr(STATEXTOID, tup,
								Anum_pg_statistic_ext_stxstattarget, &isnull);
	return (isnull ? -1 : DatumGetInt16(dat));
}


#define getAlterStatsStxstattarget_compat(a) ((Node *) makeInteger(a))
#define getIntStxstattarget_compat(a) (intVal(a))

#define WaitEventSetTracker_compat CurrentResourceOwner

#define identitySequenceRelation_compat(a) (a)

#define matched_compat(a) (a->matchKind == MERGE_WHEN_MATCHED)

#define getProcNo_compat(a) (a->vxid.procNumber)
#define getLxid_compat(a) (a->vxid.lxid)

#else

#define Anum_pg_collation_colllocale Anum_pg_collation_colliculocale
#define Anum_pg_database_datlocale Anum_pg_database_daticulocale


static inline int
getAttstattarget_compat(HeapTuple attTuple)
{
	return ((Form_pg_attribute) GETSTRUCT(attTuple))->attstattarget;
}


static inline int
getStxstattarget_compat(HeapTuple tup)
{
	return ((Form_pg_statistic_ext) GETSTRUCT(tup))->stxstattarget;
}


#define getAlterStatsStxstattarget_compat(a) (a)
#define getIntStxstattarget_compat(a) (a)

#define WaitEventSetTracker_compat CurrentMemoryContext

#define identitySequenceRelation_compat(a) (RelationGetRelid(a))

#define matched_compat(a) (a->matched)

#define create_foreignscan_path_compat(a, b, c, d, e, f, g, h, i, j, \
									   k) create_foreignscan_path(a, b, c, d, e, f, g, h, \
																  i, k)

#define getProcNo_compat(a) (a->pgprocno)
#define getLxid_compat(a) (a->lxid)

#define COLLPROVIDER_BUILTIN 'b'

#endif

#define SetListCellPtr(a, b) ((a)->ptr_value = (b))
#define RangeTableEntryFromNSItem(a) ((a)->p_rte)
#define fcGetArgValue(fc, n) ((fc)->args[n].value)
#define fcGetArgNull(fc, n) ((fc)->args[n].isnull)
#define fcSetArgExt(fc, n, val, is_null) \
		(((fc)->args[n].isnull = (is_null)), ((fc)->args[n].value = (val)))
#define fcSetArg(fc, n, value) fcSetArgExt(fc, n, value, false)
#define fcSetArgNull(fc, n) fcSetArgExt(fc, n, (Datum) 0, true)

#define CREATE_SEQUENCE_COMMAND \
		"CREATE %sSEQUENCE IF NOT EXISTS %s AS %s INCREMENT BY " INT64_FORMAT \
		" MINVALUE " INT64_FORMAT " MAXVALUE " INT64_FORMAT \
		" START WITH " INT64_FORMAT " CACHE " INT64_FORMAT " %sCYCLE"

#pragma GCC diagnostic pop
}

// CUSTOM UTILS

#include "utils.hpp"
#include "utils/logging.h"

// DEBUG UTILS


__attribute__((always_inline))
static inline Relation safe_relation_open(Oid relationId, LOCKMODE lockmode) {
	elog(NOTICE, "Columnar [safe_relation_open] " __FILE__ ":" TOSTRING(__LINE__));
	const Relation rel = relation_open(relationId, lockmode);
	elog(NOTICE, "Columnar [safe_relation_open] [SUCCESS] " __FILE__ ":" TOSTRING(__LINE__));
	return rel;
}

__attribute__((always_inline))
static inline Relation try_safe_relation_open(Oid relationId, LOCKMODE lockmode) {
	elog(NOTICE, "Columnar [try_safe_relation_open] " __FILE__ ":" TOSTRING(__LINE__));
	const Relation rel = try_relation_open(relationId, lockmode);
	elog(NOTICE, "Columnar [safe_relation_open] [SUCCESS] " __FILE__ ":" TOSTRING(__LINE__));
	return rel;
}


#endif   /* PG_VERSION_COMPAT_H */
