/** Static-target facade for the lexicographic minimizer hasher. */
module;

#include <hwy/highway.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

export module bioseq.sequences.kmers.hashers.lexic;

import bioseq.sequences.kmers.hashers.base;
import bioseq.common.sequan_compat;
import bioseq.sequences;

// Preserve the independent static API by replaying the canonical owner only
// after Highway has fixed HWY_TARGET for this module. The sole identifier
// mapping is the attributed namespace placeholder itself.
HWY_BEFORE_NAMESPACE();

#define BIOSEQ_HASHER_LEXIC_STATIC_FACADE 1
#define target HWY_NAMESPACE
#include "highway_lexic_kernel.cppm"
#undef target
#undef BIOSEQ_HASHER_LEXIC_STATIC_FACADE

HWY_AFTER_NAMESPACE();

export namespace bioseq::simd {
using HWY_NAMESPACE::lexic_hasher;
} // namespace bioseq::simd
