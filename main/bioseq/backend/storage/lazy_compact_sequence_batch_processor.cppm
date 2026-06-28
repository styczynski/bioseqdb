/*-------------------------------------------------------------------------
 *
 * storage/lazy_compact_sequence_batch_processor.cppm
 *
 * `lazy_compact_sequence_batch_processor<R, Scanner>` — the algorithm-side
 * hook the `BioseqBatchSortScan` custom scan calls to order a batch of
 * sequences (see backend/pg/table/batch_sort_scan.cppm). It batches a span
 * of `lazy_compact_sequence<R, Scanner>` and returns the output ordering
 * (a permutation) for those sequences.
 *
 * SCOPE — the real ordering algorithm is intentionally out of scope. The
 * value of this file is the *interface* plus a verifiable placeholder:
 *
 *   - A `lazy_compact_sequence` can be larger than RAM: the FRAGMENTED
 *     scanner (`sequence_scanner_pg`) streams fixed-size fragments on demand
 *     with aggressive eviction, so RSS is bounded by the fragment buffer, not
 *     by sequence length (DOC0263 R12). A real comparator would therefore
 *     stream both operands letter-by-letter through the Scanner — exactly the
 *     `iterable<BP>()` pattern `tally_symbol_counts` uses (symbol_counts_index
 *     .cppm). The processor is templated on `Scanner`, so swapping
 *     `sequence_scanner_none` (in-RAM) for `sequence_scanner_pg` (out-of-core)
 *     is mechanical — no change to the interface or the custom scan.
 *
 *   - The placeholder `lazy_compact_sequence_batch_sort` returns the batch in
 *     REVERSE order instead of sorting. This makes the planner + executor
 *     wiring verifiable end-to-end via SQL: the emitted row order becomes the
 *     exact reverse of the scan order, an unambiguous signal that our node ran
 *     (a core Sort would emit ascending/descending order, never reverse-of-
 *     scan order).
 *
 * NO_POSTGRES — this module references no Postgres internals; it lives in the
 * shared nopg module library and depends only on the lazy-sequence/scanner
 * abstractions (so the processor is unit-testable without a running server).
 *
 *-------------------------------------------------------------------------
 */
module;

// DOC0192 — anchor <hwy/highway.h> before importing the HWY-wrapped simd BMI
// (cpu_sort_sequences calls bioseq::simd::N_NEON::compare_sequences_scalar).
#include "common_simd.h"
#include "common.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.storage.batch_processor;

import bioseq.storage.lazy;        // lazy_compact_sequence
import bioseq.storage.scanner;     // is_valid_sequence_scanner, sequence_scanner_none
import bioseq.sequences;           // supports_compact_sequence_range, compact_sequence_range
import bioseq.sequences.simd;      // N_NEON::compare_sequences_scalar (CPU fallback)
import bioseq.parallelism.pool;    // GlobalPool::parallel_sort
// DOC0346 — the GPU sort façade. NB: this is folded INTO this module (rather than
// the spec's separate `bioseq.gpu.sort` module) because adding a brand-new module
// TU trips a CMake-4.3.1 `import std` generate-time toolchain re-detection bug on
// this host; editing an existing module TU does not. The façade still lives in
// its own `bioseq::gpu::sort` namespace and the processor consumes it across that
// boundary, so the separation-of-concerns intent (§7.1) is preserved in source.
import bioseq.gpu.vulkan.runtime;            // Runtime, SeqSortKernelParams, SeqDescWire, VulkanRuntimeError
import bioseq.gpu.vulkan.phase_b_abi;        // AlphabetKernelKind
import bioseq.sequences.kmers.vulkan;        // SequenceUploadView, make_sequence_upload_view, choose_alphabet_kernel, round_up4
import bioseq.parallelism.memory_admission;  // MemoryAdmissionController, saturating_add/_mul
import bioseq.common.mlogging;               // bioseq_log

// ===========================================================================
// DOC0346 — GPU sort façade (folded in; see the import note above).
// ===========================================================================
export namespace bioseq::gpu::sort {

/// Reverse the ascending permutation for DESC (§7.4). Because the device order
/// makes the original row index the secondary key, the reverse is identical to
/// the CPU's symmetric DESC tiebreak even on duplicate sequences.
[[nodiscard]] inline std::vector<std::size_t>
apply_direction(std::vector<std::size_t> perm, bool descending) {
    if (descending) std::reverse(perm.begin(), perm.end());
    return perm;
}

/// Order an in-RAM lazy batch on the GPU. Returns nullopt on any decline (GUC
/// off, no usable device, oversize), in which case the caller uses the CPU path.
/// `Spec` is duck-typed: it must expose `.vulkan_enabled` and `.descending`.
template <class LazyBatch, class Spec>
[[nodiscard]] std::optional<std::vector<std::size_t>>
gpu_sort_sequences(const LazyBatch& batch, const Spec& spec) {
    using lazy_t  = std::ranges::range_value_t<LazyBatch>;
    using range_t = std::remove_cvref_t<decltype(std::declval<lazy_t>().underlying())>;
    using traits  = typename range_t::traits;

    if (!spec.vulkan_enabled) return std::nullopt;
    auto& rt = bioseq::gpu::vulkan::Runtime::instance();
    if (!rt.usable()) return std::nullopt;

    const std::size_t n = std::ranges::size(batch);
    if (n <= 1) {
        std::vector<std::size_t> order(n);
        std::iota(order.begin(), order.end(), std::size_t{0});
        return apply_direction(std::move(order), spec.descending);
    }
    // The 0xffffffff bitonic sentinel + the uint32 GLSL indices require n and
    // every bp_size below UINT32_MAX (§6.3 / §7.2).
    if (n >= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) return std::nullopt;

    const bioseq::gpu::vulkan::SeqSortKernelParams params{
        static_cast<std::uint32_t>(bioseq::gpu::vulkan::choose_alphabet_kernel<range_t>()),
        static_cast<std::uint32_t>(traits::unit_width),
        static_cast<std::uint32_t>(traits::bits_per_char),
        static_cast<std::uint32_t>(traits::letters_per_unit),
        static_cast<std::uint32_t>(traits::char_mask),
        static_cast<std::uint64_t>(traits::used_storage_mask)};

    const std::uint32_t lpu = static_cast<std::uint32_t>(traits::letters_per_unit);
    const std::uint32_t units_for_64 =
        std::max<std::uint32_t>(1u, (64u + params.unit_bits - 1u) / params.unit_bits);
    const std::size_t unit_bytes = sizeof(typename traits::storage_unit);

    struct packed_sort_batch {
        std::vector<bioseq::gpu::vulkan::SeqSortUploadChunk> chunks;
        std::vector<bioseq::gpu::vulkan::SeqDescWire> descs;
        std::uint64_t packed_bytes = 0;
    };

    auto pack_batch = [&](std::optional<std::uint32_t> max_units)
        -> std::optional<packed_sort_batch> {
        packed_sort_batch out;
        out.chunks.reserve(n);
        out.descs.reserve(n);
        std::uint64_t total_padded_bytes = 0;
        for (const auto& lz : batch) {
            const auto view = bioseq::gpu::vulkan::make_sequence_upload_view(lz.underlying());
            if (view.bp_size > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
                return std::nullopt;
            if (view.byte_size > 0u && view.bytes == nullptr)
                return std::nullopt;
            const std::uint32_t bp = static_cast<std::uint32_t>(view.bp_size);
            const std::uint32_t unit_count = (bp + lpu - 1u) / lpu;
            const std::uint32_t upload_units =
                max_units ? std::min<std::uint32_t>(unit_count, *max_units) : unit_count;
            const std::uint64_t upload_bp64 = std::min<std::uint64_t>(
                view.bp_size,
                static_cast<std::uint64_t>(upload_units) * static_cast<std::uint64_t>(lpu));
            const std::uint64_t upload_bytes64 = std::min<std::uint64_t>(
                view.byte_size,
                static_cast<std::uint64_t>(upload_units) * static_cast<std::uint64_t>(unit_bytes));
            const std::uint64_t padded_bytes =
                bioseq::gpu::vulkan::round_up4(upload_bytes64);
            if (padded_bytes == (std::numeric_limits<std::uint64_t>::max)())
                return std::nullopt;
            const std::uint64_t word_off64 = total_padded_bytes / 4u;
            if (word_off64 > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
                return std::nullopt;
            if (total_padded_bytes > (std::numeric_limits<std::uint64_t>::max)() - padded_bytes)
                return std::nullopt;
            total_padded_bytes += padded_bytes;
            if (total_padded_bytes >
                static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
                return std::nullopt;
            out.descs.push_back(bioseq::gpu::vulkan::SeqDescWire{
                static_cast<std::uint32_t>(word_off64),
                upload_units,
                static_cast<std::uint32_t>(upload_bp64),
                0u});
            out.chunks.push_back(bioseq::gpu::vulkan::SeqSortUploadChunk{
                view.bytes,
                upload_bytes64,
                padded_bytes});
        }
        out.packed_bytes = total_padded_bytes;
        return out;
    };

    auto run_packed = [&](const packed_sort_batch& candidate, bool log_errors)
        -> std::optional<std::vector<std::uint32_t>> {
        const std::uint64_t per_row = 8u + 8u + 16u + 4u + 256u; /* keys+vals+scr+index+hist slack */
        const std::uint64_t reserve = bioseq::parallelism::saturating_add(
            candidate.packed_bytes,
            bioseq::parallelism::saturating_mul(static_cast<std::uint64_t>(n), per_row));
        auto permit = bioseq::parallelism::MemoryAdmissionController::instance().try_reserve(reserve);
        if (!permit) return std::nullopt;
        try {
            return rt.run_sequence_sort_roundtrip(candidate.chunks, candidate.descs, params);
        } catch (const bioseq::gpu::vulkan::VulkanRuntimeError& e) {
            if (log_errors)
                bioseq_log<ERR>("[gpu.sort] {}", e.what());
            return std::nullopt;
        }
    };

    auto materialize_order = [&](const std::vector<std::uint32_t>& perm32) {
        std::vector<std::size_t> order(perm32.size());
        for (std::size_t i = 0; i < perm32.size(); ++i)
            order[i] = static_cast<std::size_t>(perm32[i]);
        return apply_direction(std::move(order), spec.descending);
    };

    auto sampled_first_key_varies = [&]() -> bool {
        const auto first_view =
            bioseq::gpu::vulkan::make_sequence_upload_view(batch[0].underlying());
        if (first_view.byte_size < unit_bytes || first_view.bytes == nullptr)
            return false;
        std::vector<std::byte> first(unit_bytes);
        std::memcpy(first.data(), first_view.bytes, unit_bytes);
        const std::size_t sample = std::min<std::size_t>(n, 64u);
        for (std::size_t i = 1; i < sample; ++i) {
            const auto view =
                bioseq::gpu::vulkan::make_sequence_upload_view(batch[i].underlying());
            if (view.byte_size >= unit_bytes && view.bytes != nullptr
                && std::memcmp(first.data(), view.bytes, unit_bytes) != 0) {
                return true;
            }
        }
        return false;
    };

    // If the first 64 bits are GPU-proven to be a dense [0,n) permutation, the
    // first differing unit determines lexicographic order. Uploading only that
    // prefix avoids moving long tails for the common front-key sort case. The
    // trimmed result is accepted only when the runtime's dense-scatter counter
    // proves that the GPU dense path succeeded; otherwise it is discarded and
    // the full-payload path below is used.
    if (sampled_first_key_varies()) {
        if (auto probe_batch = pack_batch(units_for_64)) {
            const std::uint64_t dense_before = rt.seq_sort_dense_scatter_dispatch_count();
            if (auto probe_perm = run_packed(*probe_batch, false)) {
                const std::uint64_t dense_after = rt.seq_sort_dense_scatter_dispatch_count();
                if (dense_after > dense_before) {
                    return materialize_order(*probe_perm);
                }
            }
        }
    }

    auto full_batch = pack_batch(std::nullopt);
    if (!full_batch) return std::nullopt;
    auto perm32 = run_packed(*full_batch, true);
    if (!perm32) return std::nullopt;
    return materialize_order(*perm32);
}

}  // namespace bioseq::gpu::sort

export namespace bioseq::storage {

/*
 * Ordering parameters mirrored from the SQL `ORDER BY` clause. Kept as a small
 * value type so the custom scan can forward the planner's sort direction and
 * GPU enablement without coupling this storage module to Postgres types.
 */
struct batch_sort_spec {
    bool descending  = false;   //< ORDER BY ... DESC
    bool nulls_first = false;   //< NULLS FIRST
    bool vulkan_enabled = false;//< DOC0346: GPU sort allowed (bioseq.aligner.vulkan_enabled).
                                //  PG-free value type; the scan node sets it from the GUC.
};

/*
 * Batches lazy sequences and decides their output order.
 *
 *   R       : the compact-sequence range brand (any alphabet — generic per
 *             the bioseq guidelines; never hardcoded to dna4).
 *   Scanner : the lazy back-end. `sequence_scanner_none` keeps the bytes in
 *             RAM (zero-overhead pointer wrapper); `sequence_scanner_pg`
 *             streams fragments from columnar storage for larger-than-RAM
 *             sequences. The processor is identical for both.
 *   Batch   : a sized, random-access view of the lazy wrappers (a std::span).
 */
template<
    supports_compact_sequence_range R,
    is_valid_sequence_scanner Scanner,
    typename Batch = std::span<lazy_compact_sequence<R, Scanner>>
>
    requires (std::ranges::sized_range<Batch> &&
              std::ranges::random_access_range<Batch>)
struct lazy_compact_sequence_batch_processor {
    using seq_t   = std::ranges::range_value_t<Batch>;
    using batch_t = Batch;

    /*
     * CPU fallback (DOC0346 §8): order the batch with the EXACT scalar
     * comparator the btree/`Sort` use (`compare_sequences_scalar`), via
     * `GlobalPool::parallel_sort` (TBB). The order is a TOTAL order whose
     * secondary key is the original row index, with the SAME direction applied
     * to both keys (symmetric DESC tiebreak, §7.4) so the GPU and CPU
     * permutations are bit-identical even on duplicate sequences. Explicit
     * captures (no `[&]`, guideline §5).
     *
     * This is also the fallback path for explicit GPU declines. Oversized
     * equal-key runs are refined on device in the GPU runtime.
     */
    [[nodiscard]] static std::vector<std::size_t>
    cpu_sort_sequences(const Batch& batch, const batch_sort_spec& spec) {
        const std::size_t n = std::ranges::size(batch);
        std::vector<std::size_t> order(n);
        std::iota(order.begin(), order.end(), std::size_t{0});
        const bool desc = spec.descending;
        auto less = [&batch, desc](std::size_t i, std::size_t j) {
            const std::strong_ordering c =
                bioseq::simd::N_NEON::compare_sequences_scalar(batch[i].underlying(),
                                                               batch[j].underlying());
            if (c != 0) return desc ? c > 0 : c < 0;
            return desc ? i > j : i < j;   // symmetric tiebreak (§7.4)
        };
        bioseq::parallelism::GlobalPool::instance().parallel_sort(order.begin(), order.end(), less);
        return order;
    }

    /*
     * Return a permutation `order` of [0, batch.size()): the executor emits
     * batch element `order[i]` as its i-th output row.
     *
     * DOC0346 — REAL ordering. For the in-RAM (`sequence_scanner_none`)
     * specialisation the GPU façade is attempted first (it declines to nullopt
     * when the GUC is off, no device is usable, or the batch exceeds device
     * limits); otherwise the CPU comparator orders the batch. The fragmented
     * (out-of-core) specialisation skips the GPU and uses the CPU streaming
     * comparator (DOC0343 §8, orthogonal).
     *
     * Returning a permutation rather than reordering the span in place is
     * deliberate on two counts: (1) the executor must reorder whole tuples —
     * every projected column travels with its sequence, not just the seq
     * Datum; (2) the FRAGMENTED `lazy_compact_sequence` specialisation is
     * non-movable (it owns its Scanner + fragment cache), so a sort that
     * swaps wrappers could not compile for it anyway. A permutation works
     * uniformly for both specialisations.
     */
    [[nodiscard]] static std::vector<std::size_t>
    lazy_compact_sequence_batch_sort(const Batch& batch,
                                     const batch_sort_spec& spec = {}) {
        const std::size_t n = std::ranges::size(batch);
        std::vector<std::size_t> order(n);
        std::iota(order.begin(), order.end(), std::size_t{0});
        if (n <= 1) return order;
        if constexpr (!Scanner::supports_fragments()) {   // in-RAM only
            if (auto gpu = bioseq::gpu::sort::gpu_sort_sequences(batch, spec)) {
                return std::move(*gpu);
            }
        }
        return cpu_sort_sequences(batch, spec);
    }
};

}  // namespace bioseq::storage
