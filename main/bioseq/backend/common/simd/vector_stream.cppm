module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"

#include <array>
#include <type_traits>
#include <utility>

export module bioseq.simd.vector_stream;

import bioseq.simd.utils;

// Keep the independent static API, but compile it from the canonical marked
// implementation. `target` is the only token that differs between the two
// owners; the module/import preamble in the canonical file is guarded while
// it is embedded here.
HWY_BEFORE_NAMESPACE();

#define BIOSEQ_VECTOR_STREAM_STATIC_FACADE 1
#define target HWY_NAMESPACE
#include "highway_vector_stream.cppm"
#undef target
#undef BIOSEQ_VECTOR_STREAM_STATIC_FACADE

HWY_AFTER_NAMESPACE();
