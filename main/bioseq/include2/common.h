#pragma once
#ifndef DUPADUPA
#define DUPADUPA

#include <cstdint>

#include "./headers/logging.h"
#ifndef NO_POSTGRES
#include "./headers/pg_compat.h"
#else
#define elog(...)
#endif

#include "./minimap2stuff.h"

#include <libassert/assert.hpp>

/*
 * easy_profiler has been removed (Part 48.7 of NEW_PROFILING_RESEARCH.md).
 * The macros below stay as no-ops only because the upstream minimap2
 * reference under bench/tool_minimap2 still expands them; that code is
 * treated as a black box and is not part of bioseq.dylib. The bioseq
 * tree itself uses bioseq::instr::region / loop instead.
 */
#define EASY_BLOCK(...)
#define EASY_PROFILER_ENABLE
#define EASY_END_BLOCK
#define EASY_VIN(...)
#define EASY_VAR(...)
#define EASY_FUNCTION(...)

#endif
