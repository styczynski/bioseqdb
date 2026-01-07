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
  return Int32GetDatum((int32_t) 999999);
}

CXX_PG_FUNCTION_INFO_V1(mmsearch_support);
Datum mmsearch_support(PG_FUNCTION_ARGS) {
  JustLog("support -> mmsearch_support");
  Node* ret = nullptr;
  Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
  if (IsA(rawreq, SupportRequestCost))
  {
    SupportRequestCost* r = (SupportRequestCost*)rawreq;
    JustLog("support -> cost request support ");
    PlannerInfo* root = r->root;
    if (root == nullptr) {
      JustLog("support -> root is none");
    } else {
      JustLog("support -> dump some data!");

      JustLog("invoked from : [{}]", nodeStr(r->node));
      if (root->non_recursive_path == nullptr) {
        JustLog("support -> fuck");
      } else {
        JustLog("pathtytpe??? {}", (int)root->non_recursive_path->pathtype);
      }
      // for (PathKey* s : PList<PathKey>(root->path)) {
      //   //JustLog("support -> what node {}", nodeStr(s));
        
      // }
    }
    r->startup = 100000;
    r->per_tuple = 0;
    PG_RETURN_POINTER(r);
  }
  PG_RETURN_POINTER(ret);
}
}