/**
* \file
 */
#ifdef __cplusplus
#pragma once
#endif

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-attributes"
#endif
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wregister"
#endif
#ifdef __cplusplus
extern "C" {
#endif

// clang-format off
#include <postgres.h>
#include <fmgr.h>
// clang-format on
#include <access/amapi.h>
#include <access/tableam.h>
#include <access/tsmapi.h>
#include <access/tupdesc.h>
#include <catalog/namespace.h>
#include <catalog/pg_class.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_type.h>
#include <commands/event_trigger.h>
#include <executor/spi.h>
#include <foreign/fdwapi.h>
#include <funcapi.h>
#include <miscadmin.h>
#include <nodes/execnodes.h>
#include <nodes/extensible.h>
#include <nodes/memnodes.h>
#if __has_include(<nodes/miscnodes.h>)
#include <nodes/miscnodes.h>
#endif
#include <nodes/nodeFuncs.h>
#include <nodes/nodes.h>
#include <nodes/parsenodes.h>
#include <nodes/pathnodes.h>
#include <nodes/replnodes.h>
#include <nodes/supportnodes.h>
#include <nodes/tidbitmap.h>
#include <parser/analyze.h>
#include <parser/parse_func.h>
#include <parser/parser.h>
#include <storage/ipc.h>
#include <utils/acl.h>
#include <utils/builtins.h>
#include <utils/expandeddatum.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/snapmgr.h>
#include <utils/syscache.h>
#include <utils/tuplestore.h>
#include <utils/typcache.h>
#ifdef __cplusplus
}
#endif
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
