/*-------------------------------------------------------------------------
 *
 * mod.c
 *
 * This file contains module-level definitions.
 *
 * Copyright (c) 2016, Citus Data, Inc.
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#include "pg_version_compat.h"
#include "utils/logging.h"

#include "columnar/columnar.h"
#include "columnar/columnar_tableam.h"

extern "C" {
PG_MODULE_MAGIC;

void _PG_init(void);

void
_PG_init(void)
{
  log_configure();
	columnar_init();
}

CXX_PG_FUNCTION_INFO_V1(mmsearch);
Datum mmsearch(PG_FUNCTION_ARGS) {
  // Nothing here
  return BoolGetDatum(true);
}
}