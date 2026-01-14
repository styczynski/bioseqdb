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

#endif