#pragma once
#ifndef DUPADUPA
#define DUPADUPA

#include <cstdint>

#ifndef NO_POSTGRES
#include "./headers/pg_compat.h"
#else
#define elog(...)
#endif

#include <libassert/assert.hpp>

// Bioseq instrumentation uses the canonical instrumentation module, while the
// bench/tool_minimap2 sources use BENCH_INSTR_BLOCK from
// `bench/tool_minimap2/minimap2/instr_shim.h`. No EASY_* macro should
// be referenced anywhere; if a future paste of upstream minimap2 brings
// one back, fail the build instead of silently shimming it.

#endif
