/*-------------------------------------------------------------------------
 *
 * bioseq_indexscan.h
 *	Custom scan method for index
 * 
 * IDENTIFICATION
 *	src/backend/columnar/columnar_indexscan.c
 *
 *-------------------------------------------------------------------------
 */


#ifndef COLUMNAR_INDEXSCAN_H
#define COLUMNAR_INDEXSCAN_H

#include "pg_version_compat.h"


extern CustomScan * bioseq_create_indexscan_node(void);
extern void bioseq_register_indexscan_node(void);

#endif