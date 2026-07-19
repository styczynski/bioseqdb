/*-------------------------------------------------------------------------
 *
 * storage/lazy_compact_sequence_batch_processor.cppm
 *
 * `lazy_compact_sequence_batch_processor<Alphabet, Batch>` — the algorithm-side
 * hook the `BioseqBatchSortScan` custom scan calls to order a batch of
 * sequences (see backend/pg/table/batch_sort_scan.cppm). It batches a span
 * of canonical lazy-sequence views and returns the output ordering
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
// (cpu_sort_sequences calls bioseq::simd::active_target::compare_sequences_scalar).
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

import bioseq.common.sequan_compat;
import bioseq.storage.lazy;        // canonical basic_lazy_sequence_view
import bioseq.storage.scanner;
import bioseq.sequences;
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
    if (descending) {
        std::reverse(perm.data(), perm.data() + perm.size());
    }
    return perm;
}

/// Order an in-RAM lazy batch on the GPU. Returns nullopt on any decline (GUC
/// off, no hardware device, oversize), in which case the caller uses the CPU path.
/// `Spec` is duck-typed: it must expose `.vulkan_enabled` and `.descending`.
template <class LazyBatch, class Spec>
[[nodiscard]] std::optional<std::vector<std::size_t>>
gpu_sort_sequences(const LazyBatch& batch, const Spec& spec) {
    using lazy_t  = std::ranges::range_value_t<LazyBatch>;
    using view_t = std::remove_cvref_t<lazy_t>;
    using alphabet_t = typename view_t::alphabet_type;
    using traits = typename view_t::traits;
    using descriptor_t = typename view_t::resident_packed_region;
    static_assert(bioseq::storage::supports_resident_packed_probe<view_t>);

    if (!spec.vulkan_enabled) return std::nullopt;
    auto& rt = bioseq::gpu::vulkan::Runtime::instance();
    if (!bioseq::gpu::vulkan::hardware_device_available(rt)) return std::nullopt;

    const std::size_t n = std::ranges::size(batch);
    if (n <= 1) {
        std::vector<std::size_t> order(n);
        std::iota(order.data(), order.data() + order.size(), std::size_t{0});
        return apply_direction(std::move(order), spec.descending);
    }
    // The 0xffffffff bitonic sentinel + the uint32 GLSL indices require n and
    // every bp_size below UINT32_MAX (§6.3 / §7.2).
    if (n >= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) return std::nullopt;

    // The GPU accepts only one exact, stable packed layout for the whole
    // batch. Probe without flattening; source/fragmented facades and mixed
    // physical widths decline to the synchronous CPU rank-stream route.
    std::uint8_t resident_width = 0u;
    std::vector<descriptor_t> resident;
    for (const std::uint8_t width : {8u, 4u, 2u, 1u}) {
        std::vector<descriptor_t> candidate;
        candidate.reserve(n);
        bool compatible = true;
        for (const view_t view : batch) {
            if (!lazy_is_worker_shareable(view)) {
                compatible = false;
                break;
            }
            auto descriptor = view.try_resident_packed(
                0u, view.bp_size(), width);
            if (!descriptor.has_value()
                || descriptor->leading_symbol_offset != 0u) {
                compatible = false;
                break;
            }
            candidate.push_back(*descriptor);
        }
        if (compatible) {
            resident_width = width;
            resident = std::move(candidate);
            break;
        }
    }
    if (resident_width == 0u) return std::nullopt;

    constexpr std::uint32_t bits_per_symbol = static_cast<std::uint32_t>(
        traits::logical_bits_per_symbol);
    const std::uint32_t unit_bits = resident_width * 8u;
    const std::uint32_t letters_per_unit = unit_bits / bits_per_symbol;
    const std::uint64_t char_mask =
        (std::uint64_t{1} << bits_per_symbol) - 1u;
    const std::uint32_t used_bits = letters_per_unit * bits_per_symbol;
    const std::uint64_t used_storage_mask = used_bits == 64u
        ? (std::numeric_limits<std::uint64_t>::max)()
        : (std::uint64_t{1} << used_bits) - 1u;
    constexpr auto alphabet_kind = [] {
        if constexpr (seqan3::nucleotide_alphabet<alphabet_t>
            && traits::logical_bits_per_symbol == 2u
            && traits::alphabet_size == 4u) {
            return bioseq::gpu::vulkan::AlphabetKernelKind::dna4_nt4;
        } else if constexpr (seqan3::nucleotide_alphabet<alphabet_t>
            && traits::logical_bits_per_symbol == 3u
            && traits::alphabet_size == 5u) {
            return bioseq::gpu::vulkan::AlphabetKernelKind::dna5_nt4;
        } else {
            return bioseq::gpu::vulkan::AlphabetKernelKind::generic;
        }
    }();

    const bioseq::gpu::vulkan::SeqSortKernelParams params{
        static_cast<std::uint32_t>(alphabet_kind),
        unit_bits,
        bits_per_symbol,
        letters_per_unit,
        static_cast<std::uint32_t>(char_mask),
        used_storage_mask};

    const std::uint32_t lpu = letters_per_unit;
    const std::uint32_t units_for_64 =
        std::max<std::uint32_t>(1u, (64u + params.unit_bits - 1u) / params.unit_bits);
    const std::size_t unit_bytes = resident_width;

    struct packed_sort_batch {
        std::vector<bioseq::gpu::vulkan::SeqSortUploadChunk> chunks;
        std::vector<bioseq::gpu::vulkan::SeqDescWire> descs;
        std::uint64_t packed_bytes = 0;
    };

    auto pack_batch = [n, lpu, unit_bytes, &resident](
        std::optional<std::uint32_t> max_units)
        -> std::optional<packed_sort_batch> {
        packed_sort_batch out;
        out.chunks.reserve(n);
        out.descs.reserve(n);
        std::uint64_t total_padded_bytes = 0;
        for (const descriptor_t& view : resident) {
            const std::uint64_t byte_size =
                static_cast<std::uint64_t>(view.unit_count) * unit_bytes;
            if (view.bp_length > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
                return std::nullopt;
            if (byte_size > 0u && view.units == nullptr)
                return std::nullopt;
            const std::uint32_t bp = static_cast<std::uint32_t>(view.bp_length);
            const std::uint32_t unit_count = (bp + lpu - 1u) / lpu;
            const std::uint32_t upload_units =
                max_units ? std::min<std::uint32_t>(unit_count, *max_units) : unit_count;
            const std::uint64_t upload_bp64 = std::min<std::uint64_t>(
                view.bp_length,
                static_cast<std::uint64_t>(upload_units) * static_cast<std::uint64_t>(lpu));
            const std::uint64_t upload_bytes64 = std::min<std::uint64_t>(
                byte_size,
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
                view.units,
                upload_bytes64,
                padded_bytes});
        }
        out.packed_bytes = total_padded_bytes;
        return out;
    };

    auto run_packed = [&rt, params, n](
        const packed_sort_batch& candidate, bool log_errors)
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

    auto materialize_order = [&spec](
        const std::vector<std::uint32_t>& perm32) {
        std::vector<std::size_t> order(perm32.size());
        for (std::size_t i = 0; i < perm32.size(); ++i)
            order[i] = static_cast<std::size_t>(perm32[i]);
        return apply_direction(std::move(order), spec.descending);
    };

    auto sampled_first_key_varies = [&resident, unit_bytes, n]() -> bool {
        const descriptor_t& first_view = resident[0];
        const std::uint64_t first_bytes =
            static_cast<std::uint64_t>(first_view.unit_count) * unit_bytes;
        if (first_bytes < unit_bytes || first_view.units == nullptr)
            return false;
        std::vector<std::byte> first(unit_bytes);
        std::memcpy(first.data(), first_view.units, unit_bytes);
        const std::size_t sample = std::min<std::size_t>(n, 64u);
        for (std::size_t i = 1; i < sample; ++i) {
            const descriptor_t& view = resident[i];
            const std::uint64_t bytes =
                static_cast<std::uint64_t>(view.unit_count) * unit_bytes;
            if (bytes >= unit_bytes && view.units != nullptr
                && std::memcmp(first.data(), view.units, unit_bytes) != 0) {
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
 *   Alphabet : the sequence alphabet (generic per the bioseq guidelines;
 *              never hardcoded to dna4).
 *   Batch    : a sized, random-access range whose values are canonical
 *              `basic_lazy_sequence_view<Alphabet>` facades. Concrete lazy
 *              owners and their scanner backends remain outside the processor.
 */
template<
    sequence_alphabet Alphabet,
    typename Batch =
        std::span<const basic_lazy_sequence_view<Alphabet>>>
    requires (std::ranges::sized_range<Batch> &&
              std::ranges::random_access_range<Batch> &&
              std::same_as<
                  std::remove_cvref_t<std::ranges::range_value_t<Batch>>,
                  basic_lazy_sequence_view<Alphabet>>)
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
        auto* order_begin = order.data();
        auto* order_end = order_begin + order.size();
        std::iota(order_begin, order_end, std::size_t{0});
        const bool desc = spec.descending;
        auto less = [&batch, desc](std::size_t i, std::size_t j) {
            const int comparison = compare_sequence_views(
                batch[i], batch[j]);
            if (comparison != 0) {
                return desc ? comparison > 0 : comparison < 0;
            }
            return desc ? i > j : i < j;   // symmetric tiebreak (§7.4)
        };
        const bool worker_shareable = std::ranges::all_of(
            batch, [](seq_t view) {
                return lazy_is_worker_shareable(view);
            });
        if (worker_shareable) {
            bioseq::parallelism::GlobalPool::instance().parallel_sort(
                order_begin, order_end, less);
        } else {
            std::stable_sort(order_begin, order_end, less);
        }
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
        std::iota(order.data(), order.data() + order.size(), std::size_t{0});
        if (n <= 1) return order;
        if (auto gpu = bioseq::gpu::sort::gpu_sort_sequences(batch, spec)) {
            return std::move(*gpu);
        }
        return cpu_sort_sequences(batch, spec);
    }
};

}  // namespace bioseq::storage
