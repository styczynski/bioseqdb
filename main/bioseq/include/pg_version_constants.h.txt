/*-------------------------------------------------------------------------
 *
 * pg_version_constants.h
 *	   pg version related constants.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_VERSION_CONSTANTS
#define PG_VERSION_CONSTANTS

#undef PG_VERSION_NUM
#define PG_VERSION_NUM 170000

#define DEPRECATED_PG_VERSION_13 160000
#define DEPRECATED_PG_VERSION_14 160000
#define DEPRECATED_PG_VERSION_15 160000


#define PG_VERSION_16 160000
#define PG_VERSION_17 170000
#define PG_VERSION_18 180000
#define PG_VERSION_19 190000

#if PG_VERSION_NUM < PG_VERSION_17
#error "Unsupported Postgres version lower than 17"
#include notexisting.h
#endif

#endif   /* PG_VERSION_CONSTANTS */
