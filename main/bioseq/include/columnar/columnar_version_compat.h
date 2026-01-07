/*-------------------------------------------------------------------------
 *
 * bioseq_version_compat.h
 *
 *  Compatibility macros for writing code agnostic to PostgreSQL versions
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef COLUMNAR_COMPAT_H
#define COLUMNAR_COMPAT_H

#include "pg_version_compat.h"

#define ColumnarProcessUtility_compat(a, b, c, d, e, f, g, h) \
	ColumnarProcessUtility(a, b, c, d, e, f, g, h)
#define PrevProcessUtilityHook_compat(a, b, c, d, e, f, g, h) \
	PrevProcessUtilityHook(a, b, c, d, e, f, g, h)
#define GetOldestNonRemovableTransactionId_compat(a, b) \
	GetOldestNonRemovableTransactionId(a)
#define ExecSimpleRelationInsert_compat(a, b, c) \
	ExecSimpleRelationInsert(a, b, c)
#define index_insert_compat(a, b, c, d, e, f, g, h) \
	index_insert(a, b, c, d, e, f, g, h)

#define ACLCHECK_OBJECT_TABLE OBJECT_TABLE

#define ExplainPropertyLong(qlabel, value, es) \
	ExplainPropertyInteger(qlabel, NULL, value, es)

#if PG_VERSION_NUM < 130000
#define detoast_attr(X) heap_tuple_untoast_attr(X)
#endif

#endif /* COLUMNAR_COMPAT_H */
