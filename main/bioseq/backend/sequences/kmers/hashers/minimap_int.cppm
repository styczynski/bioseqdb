/** Static-target facade for the minimap integer hasher. */
module;

#include <hwy/highway.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

export module bioseq.sequences.kmers.hashers.minimap_int;

import bioseq.sequences.kmers.hashers.base;
import bioseq.sequences.kmers.hashers.lexic;
import bioseq.common.sequan_compat;
import bioseq.sequences;

// The canonical attributed source owns both scalar and SIMD hasher bodies.
// This facade fixes the static target before replay, avoiding a neutral vector
// BMI while retaining the longstanding module and public type name.
HWY_BEFORE_NAMESPACE();

#define BIOSEQ_HASHER_MINIMAP_INT_STATIC_FACADE 1
#define target HWY_NAMESPACE
#include "highway_minimap_int_kernel.cppm"
#undef target
#undef BIOSEQ_HASHER_MINIMAP_INT_STATIC_FACADE

HWY_AFTER_NAMESPACE();

export namespace bioseq::simd {
using HWY_NAMESPACE::minimap_int_hasher;
} // namespace bioseq::simd
