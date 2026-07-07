/*-------------------------------------------------------------------------
 *
 * storage/sequence_size.cppm
 *
 * sequence_size_t — the canonical "any sequence position or length" type
 * for Phase A of the length-bucketed sub-table sharding redesign
 * (DOC0260). Aliased to std::uint64_t so we cover up to 1 Tbp with three
 * orders of magnitude headroom.
 *
 * This module is the leaf of the storage-manager partition: it has no
 * imports and is consumed by every other Phase-A code path that handles
 * sequence positions or lengths.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <cstdint>
#include <cstdlib>
#include <limits>

// DOC0263 §2.2 — converted from the `bioseq_storage:sequence_size` partition
// to a STANDALONE module so the NO_POSTGRES algorithm modules (simd, pipe,
// minimap_port, the new lazy_compact_sequence machinery, …) can import
// `sequence_size_t` without pulling in the Postgres-only `bioseq_storage`
// primary unit (which re-exports `:sequence_storage_manager`). `bioseq_storage`
// keeps re-exporting this module so existing `import bioseq_storage` consumers
// are unaffected. This module is now in BIOSEQ_NOPG_MODULES and compiled with
// NO_POSTGRES=1; its lone hard-error guard therefore uses bioseq_log<DBG> + abort
// instead of `bioseq_log<ERR, PG>( …)` (the guard is defensive / has no callers, so
// the change is invisible to Phase A).
export module bioseq.storage.size;

import bioseq.common.mlogging;

export namespace bioseq::storage {

/*
 * The "any sequence position or length" type. uint64 covers 1 Tbp (= 10^12
 * base pairs) with three orders of magnitude headroom — well past the
 * project-level upper bound documented in DOC0260 §3.1.
 */
using sequence_size_t = std::uint64_t;

/*
 * Cast helper that widens a sequence_size_t to int64. Never clips to int32
 * — int64 is the SQL boundary the Phase-A length functions (bioseq.len /
 * bioseq.unit_len) now return.
 */
[[nodiscard]] [[gnu::always_inline]]
inline std::int64_t seq_size_to_int64(sequence_size_t v) noexcept {
    return static_cast<std::int64_t>(v);
}

/*
 * Maximum position that the canonical MinimapSketchData<28, 15> sidecar
 * format can address. Sequences whose total bp_size exceeds this cap MUST
 * skip the sidecar build at INSERT time — DOC0260 §6.5.3 and §13.3 .
 */
constexpr sequence_size_t kMinimizerSketchMaxPosition =
    (sequence_size_t{1} << 28) - sequence_size_t{1};

/*
 * Refuses to silently truncate a sequence position that no longer fits the
 * canonical sidecar's 28-bit minimizer-position field. Used at the
 * MinimapSketchData<28, 15> sidecar boundary; the caller is expected to
 * have already gated the sidecar write on `total_bp_size <=
 * kMinimizerSketchMaxPosition`. Reaching this branch means the gate was
 * skipped, so this is a release-active hard error rather than a debug
 * assert.
 */
[[gnu::always_inline]]
inline void check_fits_in_minimizer_sketch_position(sequence_size_t v) {
    if (v > kMinimizerSketchMaxPosition) {
        bioseq_log<DBG>(
            "bioseq storage: sequence position {} exceeds the canonical "
            "minimizer sketch (MinimapSketchData<28,15>) range ({}). "
            "Callers must skip the sidecar build for sequences whose "
            "total bp_size exceeds 2^28 letters. See DOC0260 §13.3, "
            "DOC0267 D-INLINE-SIDECAR.",
            v, kMinimizerSketchMaxPosition);
        std::abort();
    }
}

}  // namespace bioseq::storage
