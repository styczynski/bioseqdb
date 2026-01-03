/*-------------------------------------------------------------------------
 *
 * columnar_vector_execution.h
 *
 * Vectorization execution function
 *
 * Copyright (c) Hydra, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef COLUMNAR_VECTOR_EXECUTION_H
#define COLUMNAR_VECTOR_EXECUTION_H

#include "pg_version_compat.h"

extern bool CheckOpExprArgumentRules(List *args);
extern bool GetVectorizedProcedureOid(Oid procedureOid, Oid *vectorizedProcedureOid);
extern List * CreateVectorizedExprList(List *exprList);
extern List * ConstructVectorizedQualList(TupleTableSlot *slot, List *vectorizedQual);
extern bool * ExecuteVectorizedQual(TupleTableSlot *slot,
									List *vectorizedQualList,
									BoolExprType boolType,
									ExprContext *econtext);

#endif
