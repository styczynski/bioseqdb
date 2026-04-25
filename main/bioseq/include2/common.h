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

// easy_profiler is fully removed (Part 48.7 of NEW_PROFILING_RESEARCH.md
// and follow-up): the bioseq tree uses bioseq::instr::region/loop and
// the bench/tool_minimap2 sources use BENCH_INSTR_BLOCK from
// `bench/tool_minimap2/minimap2/instr_shim.h`. No EASY_* macro should
// be referenced anywhere; if a future paste of upstream minimap2 brings
// one back, fail the build instead of silently shimming it.

#endif
