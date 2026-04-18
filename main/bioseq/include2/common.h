#pragma once
#ifndef DUPADUPA
#define DUPADUPA

#include <cstdint>

#include "./headers/logging.h"
#ifndef NO_POSTGRES
#include "./headers/pg_compat.h"
#endif
#include "./headers/math.h"

#ifndef NO_POSTGRES
// trash temp
#include "./trash.h"
#endif

#include "./minimap2stuff.h"

#include <libassert/assert.hpp>
#ifdef NO_POSTGRES
#include <easy/profiler.h>
#else
// FIXME: Woraround because for postgres build easy_profiler defines now() which is defined in Postgres source as well!
#define EASY_BLOCK(...)
#define EASY_PROFILER_ENABLE
#define EASY_END_BLOCK
#define EASY_VIN(...)
#define EASY_VAR(...)
#define EASY_FUNCTION(...)
#endif

#endif