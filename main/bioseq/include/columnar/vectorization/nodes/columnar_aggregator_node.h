/*-------------------------------------------------------------------------
 *
 * bioseq_aggregator_node.h
 *	Custom scan method for aggregation
 * 
 * IDENTIFICATION
 *	src/backend/columnar/vectorization/nodes/columnar_aggregator_node.c
 *
 *-------------------------------------------------------------------------
 */


#ifndef COLUMNAR_AGGEREGATOR_NODE_H
#define COLUMNAR_AGGEREGATOR_NODE_H

#include "pg_version_compat.h"

typedef struct VectorAggState
{
	CustomScanState css;
	AggState *aggstate;
} VectorAggState;

extern CustomScan *bioseq_create_aggregator_node(void);
extern void bioseq_register_aggregator_node(void);

#endif
