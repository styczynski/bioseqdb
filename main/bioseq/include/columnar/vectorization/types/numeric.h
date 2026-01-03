/*-------------------------------------------------------------------------
 *
 * numeric.h
 * 
 * IDENTIFICATION
 *	src/backend/columnar/vectorization/types/numeric.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef VECTORIZATION_TYPE_INT128_H
#define VECTORIZATION_TYPE_INT128_H

#include "pg_version_compat.h"

extern Numeric int128_to_numeric(int128 val);

#endif