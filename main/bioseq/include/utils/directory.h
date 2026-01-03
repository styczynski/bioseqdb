/*-------------------------------------------------------------------------
 *
 * directory.h
 *	  Utility functions for dealing with directories.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef CITUS_DIRECTORY_H
#define CITUS_DIRECTORY_H

#include "pg_version_compat.h"


#define PG_JOB_CACHE_DIR "pgsql_job_cache"


extern void CleanupJobCacheDirectory(void);
extern void CitusCreateDirectory(StringInfo directoryName);
extern void CitusRemoveDirectory(const char *filename);


#endif   /* CITUS_DIRECTORY_H */
