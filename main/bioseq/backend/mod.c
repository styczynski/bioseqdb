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

import alignment;

extern "C" {
PG_MODULE_MAGIC;

void _PG_init(void);

void
_PG_init(void)
{
  log_configure();
	bioseq_init();
}

CXX_PG_FUNCTION_INFO_V1(testtest);
Datum testtest(PG_FUNCTION_ARGS) {
  // Nothing here
  JustLog("TEST align?");
  auto ez = make<ksw_extz_t>();
  Aligner<ALIGN_ALGORITHM_KSW2>::z2(nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, 0, 0, 0, 0, 0, ez);
  JustLog("Align done :D");
  return BoolGetDatum(true);
}

CXX_PG_FUNCTION_INFO_V1(mmsearch);
Datum mmsearch(PG_FUNCTION_ARGS) {
  // Nothing here
  return Int32GetDatum((int32_t) 100);
}
}