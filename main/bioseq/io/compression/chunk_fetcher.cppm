/**
 * @file chunk_fetcher.cppm
 * @brief Parallel gzip chunk decoder.
 *
 * Two execution paths:
 *
 *   * **Single-thread fallback** (`worker_count <= 1`) — the proven
 *     correctness baseline. The producer thread parses gzip headers and
 *     drives `deflate::block` over each member to fill arena slots in
 *     order. ~88 MiB/s gz on Apple Silicon.
 *
 *   * **Parallel pool** (`worker_count > 1`) — the rapidgzip
 *     architecture (paper §3.2 + §5.3):
 *
 *     1. **Pre-scan members** (sequential, ~10 GB/s memory) — scan
 *        `_compressed_view` for `0x1f 0x8b 0x08 <flags>` candidates,
 *        validate with `parse_gzip_header`. Cost is invisible vs
 *        decompression (0.2 s for 1.9 GiB).
 *     2. **Plan chunks** — for each member, partition
 *        `[header_end_bit, footer_start_bit)` into ~4 MiB compressed
 *        chunks. Chunk 0 of each member starts at the first deflate
 *        block (direct mode); chunks 1..N find their start via
 *        `seek_dynamic_huffman_candidate` (marker mode).
 *     3. **Worker pool** — N threads race on `_next_worker_chunk_idx`,
 *        own a heap-allocated `block`, decode into
 *        `arena[chunk_idx % kArenaCapacity]`, advance the slot
 *        lifecycle to `READY_FOR_DRAIN`.
 *     4. **Drain** — the producer thread waits for chunks in chunk-idx
 *        order, runs the marker resolver against `_drain_window` (the
 *        last 32 KiB of resolved bytes), captures the new trailing
 *        window, resets the window at member boundaries, and publishes
 *        the chunk into the consumer's ready ring.
 *
 * **Slot lifecycle** (parallel path):
 *
 *     FREE → WORKER_FILLING → READY_FOR_DRAIN → READY_FOR_CONSUMER
 *          → CONSUMER_HOLDING → FREE
 *
 * Every transition holds `_state_mutex` and notifies `_state_cv`.
 * Workers wait for `FREE`; drain waits for `READY_FOR_DRAIN` AND the
 * slot's tagged chunk_idx matching the drain cursor. The single-thread
 * path collapses the worker+drain steps and transitions
 * `FREE → WORKER_FILLING → READY_FOR_CONSUMER` directly.
 */
module;

#include "common.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

export module bioseq_io.compression.chunk_fetcher;

import mlogging;
import bioseq_io.simd_kernels;
import bioseq_io.compression.aligned_byte_buffer;
import bioseq_io.compression.bit_reader;
import bioseq_io.compression.blockfinder;
import bioseq_io.compression.chunk;
import bioseq_io.compression.deflate;
import bioseq_io.compression.gzip_format;
import bioseq_io.compression.huffman;

export namespace bioseq::io::compression {

/// Capacity of the in-flight chunk arena. With 32 MiB compressed
/// chunks × 3.16 ratio × 2 (uint16 marker storage) = ~200 MiB peak
/// per slot. 8 slots × 200 MiB = 1.6 GiB peak — large but acceptable
/// for the 4× target. Smaller arenas starve the worker pool when
/// consumer is slow.
inline constexpr std::size_t kChunkArenaCapacity = 8;

/// Chunk target size (compressed bytes). The paper's optimum is
/// 4 MiB on a 128-core x86 system; on Apple Silicon with 6-8 workers
/// the per-chunk overheads (plan validation, slot lifecycle dance,
/// trailing-window propagation) eat a larger fraction at small chunk
/// sizes. 32 MiB cuts chunk count from ~462 to ~59 on the SRR
/// corpus, amortising those overheads while keeping arena RAM
/// bounded (8 slots × 100 MiB marker = 800 MiB peak).
inline constexpr std::size_t kDefaultChunkTargetBytes = 32 * 1024 * 1024;

/// Cap worker count. Past ~core count workers stop helping, and the
/// drain side becomes the bottleneck. M1 Pro has 8 perf cores, so 8 is
/// the practical ceiling.
inline constexpr std::size_t kMaxWorkerCount = 12;

/// Hard cap on a chunk's decompressed output size. Real chunks are
/// ~50 MiB (16 MiB compressed × 3.16 ratio). 256 MiB gives a 5× safety
/// margin against unusually-compressible cells (long runs of identical
/// bytes) while bounding the damage from a false-positive block start
/// that passes header validation but decodes to garbage. Chunks
/// exceeding this cap return an error rather than ballooning RAM.
inline constexpr std::size_t kMaxChunkDecompressedBytes = 256 * 1024 * 1024;

namespace detail {

// =====================================================================
// Slot lifecycle (parallel path)
// =====================================================================

enum class slot_lifecycle : std::uint8_t {
    FREE                = 0,
    WORKER_FILLING      = 1,
    READY_FOR_DRAIN     = 2,
    PHASE_1_DONE        = 3,  // drain finished phase-1 (last 32 KiB);
                              // phase-2 worker can resolve remainder
    READY_FOR_CONSUMER  = 4,
    CONSUMER_HOLDING    = 5,
};

// =====================================================================
// Member pre-scan
// =====================================================================

struct member_info {
    /// Byte offset of the gzip-magic byte (0x1f).
    std::size_t header_byte = 0;
    /// First bit of the first deflate block in this member (= header
    /// end bit).
    std::size_t header_end_bit = 0;
};

/// Lightweight validation of a dynamic-Huffman block header at the
/// reader's current position. Mirrors `block::read_header` but skips
/// the actual literal/distance Huffman table BUILDS — instead just
/// runs the Kraft completeness check on each table's code lengths.
/// ~5× faster than `read_header` for false-positive rejection.
[[nodiscard]] inline bool validate_dynamic_huffman_only(
    bit_reader& reader) noexcept;

/// Walk `compressed_view` linearly looking for gzip-magic candidates
/// (`0x1f 0x8b 0x08 <flags ≤ 0x1f>`). Each candidate is fed through:
///
///   1. `parse_gzip_header` — magic bytes, method, NUL-terminated
///      FNAME / FCOMMENT (when flagged).
///   2. `validate_dynamic_huffman_only` at the would-be first deflate
///      block — Kraft completeness check on the literal+distance
///      trees.
///
/// Step 2 is what filters compressed-data sequences that randomly look
/// like a gzip header. The 4-byte magic prefix `1f 8b 08 [00..1f]` has
/// 30 valid combinations across 2^32 — at ~2 GiB of compressed data,
/// expect ~4 prefilter hits PER FILE just from random data. With FNAME
/// / FCOMMENT being NUL-terminated reads on random bytes,
/// `parse_gzip_header` alone passes nearly all of them: a 1.9 GiB
/// SRR FASTQ.gz had 19 false-positive members and 1 real one.
///
/// Each fake member's chunk-0 starts in DIRECT mode at a non-block bit
/// position. The decoder reads garbage Huffman codes and eventually
/// emits a back-reference whose `distance > _decoded_bytes + produced`,
/// returning EXCEEDED_WINDOW_RANGE. The Kraft check at the candidate's
/// `header_end_bit` rejects ~5e-7 of the prefilter survivors per the
/// rapidgzip paper (tab:false_positives), bringing expected fake
/// members per file to effectively zero.
///
/// COMPATIBILITY: this rejects the rare case of a real gzip member
/// whose first deflate block is fixed-Huffman or uncompressed. Real
/// gzip encoders (gzip, pigz, igzip, zlib level ≥ 1) emit dynamic
/// Huffman for any payload past a few hundred bytes, so this is safe
/// for production data. If you need to support fixed-block first
/// members, fall back to the single-thread producer (`worker_count
/// <= 1`).
[[nodiscard]] inline std::expected<std::vector<member_info>, std::string>
prescan_members(std::span<const std::uint8_t> compressed_view) noexcept {
    std::vector<member_info> members;
    if (compressed_view.empty()) return members;

    std::size_t cursor = 0;
    while (cursor + 4 <= compressed_view.size()) {
        // SIMD-scan for the next 0x1f. After the first member, this
        // saves ~40 GB/s of byte-by-byte scan.
        const std::size_t advance = bioseq::io::simd::scan_first_byte(
            std::uint8_t{0x1F},
            compressed_view.data() + cursor,
            compressed_view.size() - cursor);
        cursor += advance;
        if (cursor + 4 > compressed_view.size()) break;

        // Cheap pre-filter: 0x1f 0x8b 0x08 <flags ≤ 0x1f>.
        if (compressed_view[cursor + 0] != 0x1F
            || compressed_view[cursor + 1] != 0x8B
            || compressed_view[cursor + 2] != 0x08
            || compressed_view[cursor + 3] > 0x1F) {
            ++cursor;
            continue;
        }

        // Step 1: parse the header itself (magic + flags + optional
        // NUL-terminated fields).
        bit_reader probe{
            std::span<const std::uint8_t>{
                compressed_view.data() + cursor,
                compressed_view.size() - cursor}};
        auto header_or = parse_gzip_header(
            probe,
            std::span<const std::uint8_t>{
                compressed_view.data() + cursor,
                compressed_view.size() - cursor});
        if (!header_or) {
            ++cursor;
            continue;
        }
        const std::size_t header_end_bit_global =
            cursor * 8u + header_or->end_bit_offset;

        // Step 2: validate the first deflate block at header_end_bit
        // is dynamic-Huffman with Kraft-complete literal + distance
        // trees. This is what distinguishes a real gzip member start
        // from a 4-byte pattern that happens to live inside compressed
        // data. Without this check, every random `1f 8b 08 <flag>`
        // hit becomes a fake "member" whose chunk-0 decodes garbage in
        // direct mode (EXCEEDED_WINDOW_RANGE downstream).
        bit_reader block_probe{compressed_view};
        block_probe.seek_to_bit(header_end_bit_global);
        if (!validate_dynamic_huffman_only(block_probe)) {
            ++cursor;
            continue;
        }

        member_info info;
        info.header_byte = cursor;
        info.header_end_bit = header_end_bit_global;
        members.push_back(info);

        // Skip past the header. The actual deflate stream starts here;
        // we'll rediscover the next member when we hit its magic.
        cursor = info.header_end_bit / 8u;
        if ((info.header_end_bit % 8u) != 0u) ++cursor;
    }

    if (members.empty()) {
        return std::unexpected(std::string{
            "prescan_members: no gzip member detected in input"});
    }
    return members;
}

// =====================================================================
// Chunk planning
// =====================================================================

struct chunk_plan {
    std::uint32_t member_idx          = 0;
    /// Bit offset of a valid deflate-block start. Worker seeks here.
    std::size_t   start_bit           = 0;
    /// Worker stops decoding once a block starts at-or-after this bit
    /// (mirrors librapidarchive's `untilOffset`).
    std::size_t   stop_at_bit         = 0;
    /// True for the very first chunk of the member (decoder runs in
    /// direct mode; window starts empty).
    bool          is_first_in_member  = false;
    /// True for the chunk that contains the BFINAL=1 block of the
    /// member. Drain resets `_drain_window_size` after it.
    bool          is_last_in_member   = false;
};

[[nodiscard]] inline bool validate_dynamic_huffman_only(
    bit_reader& reader) noexcept {
    if (reader.bits_buffered() < 3) reader.refill();
    if (reader.bits_buffered() < 3) return false;
    const std::uint64_t bfinal_btype = reader.read<3>();
    const std::uint64_t btype = (bfinal_btype >> 1) & 0x3u;
    if (btype != 0x2u) return false;

    if (reader.bits_buffered() < 14) reader.refill();
    if (reader.bits_buffered() < 14) return false;
    const std::size_t literal_count =
        static_cast<std::size_t>(reader.read<5>()) + 257u;
    if (literal_count > kMaxLiteralOrLengthSymbols) return false;
    const std::size_t distance_count =
        static_cast<std::size_t>(reader.read<5>()) + 1u;
    if (distance_count > kMaxDistanceSymbolCount) return false;
    const std::size_t precode_count =
        static_cast<std::size_t>(reader.read<4>()) + 4u;

    std::array<std::uint8_t, kMaxPrecodeCount> precode_cl{};
    for (std::size_t i = 0; i < precode_count; ++i) {
        if (reader.bits_buffered() < kPrecodeBits) reader.refill();
        // Fully-qualify `kPrecodeAlphabet` — the unqualified name
        // shadows on `bioseq::io::compression::detail::kPrecodeAlphabet`
        // (from huffman.cppm), which is a size_t count, not the
        // alphabet array (which lives in the parent namespace).
        precode_cl[::bioseq::io::compression::kPrecodeAlphabet[i]] =
            static_cast<std::uint8_t>(reader.read<kPrecodeBits>());
    }

    const auto precode = precode_decoder::build(
        std::span<const std::uint8_t>{precode_cl.data(), precode_cl.size()});
    if (!precode.is_valid()) return false;

    std::array<std::uint8_t, kLiteralAndDistanceClBufferSize> literal_cl{};
    if (auto err = read_distance_and_literal_code_lengths(
            literal_cl, reader, precode,
            literal_count + distance_count);
        err != decode_status::OK) {
        return false;
    }
    if (literal_cl[kEndOfBlockSymbol] == 0) return false;
    if (!lit_len_decoder::is_valid_code_lengths(
            std::span<const std::uint8_t>{literal_cl.data(), literal_count})) {
        return false;
    }
    if (!distance_decoder::is_valid_code_lengths(
            std::span<const std::uint8_t>{literal_cl.data() + literal_count,
                                          distance_count})) {
        return false;
    }
    return true;
}

/// Build a per-member chunk plan. For each member, the first chunk
/// starts at the header-end bit; subsequent chunks find a valid
/// dynamic-Huffman block via `seek_dynamic_huffman_candidate`. The
/// `stop_at_bit` of chunk K is the `start_bit` of chunk K+1 (or, for
/// the last chunk of a member, the next member's start or end of file).
///
/// CRITICAL: each chunk's `start_bit` MUST be a fully-validated
/// dynamic Huffman block start. Precode-only validation lets through
/// ~478M / 1e12 false positives (paper tab:false_positives). At the
/// 1.876 GiB corpus's bit count that's ~3 candidates that LOOK like
/// real blocks but produce garbage when decoded — and any one of them
/// at a chunk boundary corrupts the output stream. `read_header` runs
/// the full Kraft completeness check on literal+distance trees, which
/// is the strongest filter we have without actually decoding the body.
[[nodiscard]] inline std::expected<std::vector<chunk_plan>, std::string>
plan_chunks(std::span<const std::uint8_t>      compressed_view,
            const std::vector<member_info>&    members,
            std::size_t                        chunk_target_compressed_bytes) noexcept {
    std::vector<chunk_plan> plans;
    if (members.empty()) {
        return std::unexpected(std::string{"plan_chunks: no members"});
    }

    const std::size_t total_bits = compressed_view.size() * 8u;

    auto find_validated_block = [&](std::size_t  start_bit,
                                    std::size_t  until_bit) noexcept -> std::size_t {
        std::size_t cursor = start_bit;
        while (cursor < until_bit) {
            bit_reader scan{compressed_view};
            scan.seek_to_bit(cursor);
            const std::size_t candidate =
                seek_dynamic_huffman_candidate(scan, until_bit);
            if (candidate == kBlockfinderSentinel) {
                return kBlockfinderSentinel;
            }
            bit_reader probe{compressed_view};
            probe.seek_to_bit(candidate);
            // Lightweight Kraft check — skips the literal+distance
            // table BUILDS that `block::read_header` performs. Saves
            // ~5 µs per candidate × ~5.8M candidates per corpus → ~30 s
            // sequential, ~5 s parallel.
            if (validate_dynamic_huffman_only(probe)) {
                return candidate;
            }
            cursor = candidate + 1u;
        }
        return kBlockfinderSentinel;
    };

    // Worker count for parallel candidate validation. Plan generation
    // is dominated by `read_header` rejection at false positives —
    // ~30 µs each, ~1500 per chunk in real DEFLATE. Parallelising
    // across the same N threads we'll use for decoding amortises this
    // back into the overall pipeline.
    const unsigned hw_threads = std::thread::hardware_concurrency();
    const std::size_t plan_workers =
        std::min<std::size_t>(hw_threads > 2 ? hw_threads - 2 : 1, 8);

    for (std::size_t m = 0; m < members.size(); ++m) {
        const std::size_t member_start_bit = members[m].header_end_bit;
        const std::size_t member_end_bit =
            (m + 1u < members.size())
                ? (members[m + 1u].header_byte * 8u)
                : total_bits;
        if (member_end_bit <= member_start_bit) {
            return std::unexpected(std::string{
                "plan_chunks: member spans zero bytes"});
        }
        const std::size_t member_compressed_bits =
            member_end_bit - member_start_bit;
        const std::size_t member_compressed_bytes =
            member_compressed_bits / 8u;

        const std::size_t target_bytes =
            chunk_target_compressed_bytes == 0
                ? kDefaultChunkTargetBytes
                : chunk_target_compressed_bytes;
        std::size_t n_chunks = member_compressed_bytes / target_bytes;
        if (n_chunks == 0u) n_chunks = 1u;
        if (n_chunks > 4096u) n_chunks = 4096u;

        std::vector<std::size_t> plan_start_bits;
        plan_start_bits.reserve(n_chunks);
        plan_start_bits.push_back(member_start_bit);

        // Validate chunk start_bits IN PARALLEL across `plan_workers`
        // threads. Each thread owns a heap-allocated probe decoder and
        // searches its assigned chunk indices. Per-chunk results are
        // collected into a vector and merged afterwards. Without
        // parallelism, validation took ~20 s on the 1.876 GiB corpus
        // (paper false-positive rate × 30 µs read_header per
        // candidate). With 6 workers we land ~3-4 s.
        std::vector<std::size_t> per_chunk_start(n_chunks,
                                                 kBlockfinderSentinel);
        per_chunk_start[0] = member_start_bit;

        std::atomic<std::size_t> next_k{1};
        std::vector<std::thread> validators;
        validators.reserve(plan_workers);
        for (std::size_t w = 0; w < plan_workers; ++w) {
            validators.emplace_back([&, m]() noexcept {
                while (true) {
                    const std::size_t k = next_k.fetch_add(
                        1, std::memory_order_relaxed);
                    if (k >= n_chunks) break;
                    const std::size_t target_bit =
                        member_start_bit
                        + (k * member_compressed_bits) / n_chunks;
                    per_chunk_start[k] = find_validated_block(
                        target_bit, member_end_bit);
                }
            });
        }
        for (auto& t : validators) {
            if (t.joinable()) t.join();
        }
        // Merge in chunk-index order, skipping sentinels and duplicates.
        for (std::size_t k = 1; k < n_chunks; ++k) {
            const std::size_t candidate = per_chunk_start[k];
            if (candidate == kBlockfinderSentinel) continue;
            if (!plan_start_bits.empty()
                && plan_start_bits.back() == candidate) continue;
            plan_start_bits.push_back(candidate);
        }

        for (std::size_t k = 0; k < plan_start_bits.size(); ++k) {
            chunk_plan plan;
            plan.member_idx = static_cast<std::uint32_t>(m);
            plan.start_bit  = plan_start_bits[k];
            plan.stop_at_bit =
                (k + 1u < plan_start_bits.size())
                    ? plan_start_bits[k + 1u]
                    : member_end_bit;
            plan.is_first_in_member = (k == 0u);
            plan.is_last_in_member  = (k + 1u == plan_start_bits.size());
            plans.push_back(plan);
        }
    }

    return plans;
}

// =====================================================================
// Marker resolver
// =====================================================================

/// Resolve markers in `chunk.marker_storage` against `prev_window`
/// (the previous chunk's last `prev_window_size` bytes), writing the
/// resolved bytes into `chunk.byte_storage`. After the call,
/// `chunk.uses_marker_storage == false` and the byte_storage holds
/// `chunk.marker_storage.size` bytes.
/// Inner per-lane resolver. Resolves `count` uint16 lanes from
/// `src` into uint8 bytes at `dst`, using `prev_window` for marker
/// lookups. Branch-free; compilers auto-vectorise the arithmetic.
inline void resolve_lane_range(
    const std::uint16_t* src,
    std::uint8_t*        dst,
    std::size_t          count,
    const std::uint8_t*  prev_window,
    std::size_t          prev_window_size) noexcept {
    alignas(64) std::array<std::uint8_t, 0x8000> padded_prev{};
    if (prev_window_size > 0u) {
        std::memcpy(padded_prev.data(), prev_window,
                    std::min(prev_window_size,
                             static_cast<std::size_t>(0x8000)));
    }
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint16_t value = src[i];
        const std::uint16_t low15 = value & 0x7FFFu;
        const std::uint8_t  marker_byte = padded_prev[low15];
        const std::uint8_t  literal_byte = static_cast<std::uint8_t>(value);
        const std::uint8_t  is_marker_mask =
            static_cast<std::uint8_t>(
                -static_cast<std::int8_t>((value >> 15) & 0x1u));
        dst[i] = static_cast<std::uint8_t>(
            (is_marker_mask & marker_byte)
            | (~is_marker_mask & literal_byte));
    }
}

/// **Phase 1**: resolve the chunk's LAST 32 KiB only. Sequential
/// across chunks (each chunk's trailing window depends on the
/// previous chunk's). Saves the prior `drain_window` snapshot into
/// the chunk so phase 2 can resolve the remainder using the same
/// prev-window in parallel.
[[nodiscard]] inline bool resolve_markers_phase_1(
    chunk_data&         chunk,
    const std::uint8_t* drain_window,
    std::size_t         drain_window_size) noexcept {
    if (!chunk.uses_marker_storage) return true;
    const std::size_t n = chunk.marker_storage.size;

    auto reserve_or = chunk.byte_storage.reserve(n);
    if (!reserve_or) return false;
    auto extend_or = chunk.byte_storage.extend(n);
    if (!extend_or) return false;
    std::uint8_t* dst = extend_or->data();
    const std::uint16_t* src = chunk.marker_storage.data.get();

    chunk.prior_window_snapshot_size =
        std::min(drain_window_size, kChunkTrailingWindowBytes);
    if (chunk.prior_window_snapshot_size > 0) {
        std::memcpy(chunk.prior_window_snapshot.data(),
                    drain_window, chunk.prior_window_snapshot_size);
    }

    const std::size_t tail_n =
        std::min<std::size_t>(n, kChunkTrailingWindowBytes);
    const std::size_t tail_begin = n - tail_n;
    resolve_lane_range(src + tail_begin, dst + tail_begin, tail_n,
                       drain_window, drain_window_size);

    if (tail_n > 0u) {
        std::memcpy(chunk.trailing_window.data(),
                    dst + tail_begin, tail_n);
    }
    chunk.trailing_window_size = tail_n;
    return true;
}

/// **Phase 2**: resolve the chunk's REMAINDER (everything except
/// the last 32 KiB that phase 1 already handled). Independent across
/// chunks — runs in parallel from a worker pool. Uses the chunk's
/// `prior_window_snapshot` saved during phase 1.
inline void resolve_markers_phase_2(chunk_data& chunk) noexcept {
    if (!chunk.uses_marker_storage) {
        return;
    }
    const std::size_t n = chunk.marker_storage.size;
    if (n <= kChunkTrailingWindowBytes) {
        // Phase 1 already resolved all of it; just clear marker state.
        chunk.marker_storage.clear();
        chunk.uses_marker_storage = false;
        return;
    }

    const std::size_t prefix_n = n - kChunkTrailingWindowBytes;
    std::uint8_t* dst = chunk.byte_storage.data.get();
    const std::uint16_t* src = chunk.marker_storage.data.get();
    resolve_lane_range(src, dst, prefix_n,
                       chunk.prior_window_snapshot.data(),
                       chunk.prior_window_snapshot_size);

    chunk.marker_storage.clear();
    chunk.uses_marker_storage = false;
}

// =====================================================================
// Direct-mode buffer copy helpers
// =====================================================================

[[nodiscard]] inline std::size_t copy_direct_view(
    chunk_uint8_buffer&       byte_storage,
    const decoded_data_view&  view) noexcept {
    const std::size_t total = view.data_size();
    if (total == 0) return 0;
    auto extend_or = byte_storage.extend(total);
    if (!extend_or) return std::numeric_limits<std::size_t>::max();
    std::uint8_t* dst = extend_or->data();
    std::size_t copied = 0;
    for (const auto& part : view.data) {
        if (part.empty()) continue;
        std::memcpy(dst + copied, part.data(), part.size());
        copied += part.size();
    }
    return copied;
}

[[nodiscard]] inline std::size_t copy_marker_view(
    chunk_uint16_buffer&      marker_storage,
    const decoded_data_view&  view) noexcept {
    const std::size_t total = view.data_with_markers_size();
    if (total == 0) return 0;
    auto extend_or = marker_storage.extend(total);
    if (!extend_or) return std::numeric_limits<std::size_t>::max();
    std::uint16_t* dst = extend_or->data();
    std::size_t copied = 0;
    for (const auto& part : view.data_with_markers) {
        if (part.empty()) continue;
        std::memcpy(dst + copied, part.data(),
                    part.size() * sizeof(std::uint16_t));
        copied += part.size();
    }
    return copied;
}

/// Drive `block` over `reader` until `target_bytes` of decompressed
/// output have been emitted (or end-of-stream). Used by the
/// single-thread producer fallback.
[[nodiscard]] inline std::pair<std::size_t, decode_status>
decode_until(block&         decoder,
             bit_reader&    reader,
             chunk_data&    out_chunk,
             std::size_t    target_bytes) noexcept {
    std::size_t produced = 0;
    decode_status final_status = decode_status::OK;

    while (produced < target_bytes) {
        if (decoder.is_end_of_block()) {
            const auto err = decoder.read_header(reader);
            if (err != decode_status::OK) {
                final_status = err;
                break;
            }
        }
        const std::size_t want = std::min(
            target_bytes - produced, std::size_t{1024 * 1024});
        const auto [view, status] = decoder.read(reader, want);
        if (view.size() > 0) {
            if (view.contains_markers()) {
                out_chunk.uses_marker_storage = true;
                const std::size_t copied =
                    copy_marker_view(out_chunk.marker_storage, view);
                if (copied == std::numeric_limits<std::size_t>::max()) {
                    final_status = decode_status::GENERIC_ERROR;
                    break;
                }
                produced += copied;
            } else {
                if (out_chunk.uses_marker_storage
                    && out_chunk.marker_storage.size > 0) {
                    auto reserve_or = out_chunk.byte_storage.reserve(
                        out_chunk.marker_storage.size + view.data_size());
                    if (!reserve_or) {
                        final_status = decode_status::GENERIC_ERROR;
                        break;
                    }
                    auto extend_or = out_chunk.byte_storage.extend(
                        out_chunk.marker_storage.size);
                    if (!extend_or) {
                        final_status = decode_status::GENERIC_ERROR;
                        break;
                    }
                    std::uint8_t* dst = extend_or->data();
                    const std::uint16_t* src =
                        out_chunk.marker_storage.data.get();
                    for (std::size_t i = 0; i < out_chunk.marker_storage.size; ++i) {
                        dst[i] = static_cast<std::uint8_t>(src[i] & 0xFFu);
                    }
                    out_chunk.marker_storage.clear();
                    out_chunk.uses_marker_storage = false;
                }
                const std::size_t copied =
                    copy_direct_view(out_chunk.byte_storage, view);
                if (copied == std::numeric_limits<std::size_t>::max()) {
                    final_status = decode_status::GENERIC_ERROR;
                    break;
                }
                produced += copied;
            }
        }
        if (status != decode_status::OK) {
            final_status = status;
            break;
        }
        if (decoder.is_end_of_block() && decoder.is_last_block()) {
            break;
        }
    }
    return {produced, final_status};
}

// =====================================================================
// Worker chunk decode (parallel-pool path)
// =====================================================================

/// Decode a single chunk per a `chunk_plan`. Invoked from each worker
/// thread. The worker owns `decoder` (a heap-allocated `block`) and a
/// fresh `bit_reader` over `compressed_view`.
[[nodiscard]] inline decode_status worker_decode_chunk(
    block&                            decoder,
    std::span<const std::uint8_t>     compressed_view,
    const chunk_plan&                 plan,
    chunk_data&                       out_chunk) noexcept {
    out_chunk.byte_storage.clear();
    out_chunk.marker_storage.clear();
    out_chunk.decompressed_size_bytes = 0;
    out_chunk.uses_marker_storage = false;
    out_chunk.is_member_final = false;
    out_chunk.encoded_start_bit_offset = plan.start_bit;
    out_chunk.encoded_end_bit_offset = plan.start_bit;
    out_chunk.error_message.assign(std::string_view{});
    out_chunk.trailing_window_size = 0;

    bit_reader reader{compressed_view};
    reader.seek_to_bit(plan.start_bit);
    if (plan.is_first_in_member) {
        // Direct mode: empty prior window. This start was verified by
        // the gzip header parse during prescan — no false-positive
        // retry needed.
        decoder.reset();
        decoder.set_initial_window(std::span<const std::uint8_t>{});
    } else {
        // Marker mode: fresh decoder, prior window unknown. The
        // `plan.start_bit` was returned by `seek_dynamic_huffman_candidate`
        // (precode-only validation). The full literal+distance Kraft
        // check happens here, parallelised across all workers.
        // Per the paper tab:false_positives, the Kraft check rejects
        // ~478M precode-valid candidates per 1e12 bit positions, so
        // each chunk may need to scan past several false positives
        // before finding a real block start.
        //
        // We deliberately avoid `block::reset()` between attempts —
        // its 128 KiB marker-buffer memcpy dominates wall clock at
        // false-positive rates. `read_header` is idempotent on state
        // (it overwrites compression_type, is_last_block, and Huffman
        // tables unconditionally), so a single reset() upfront is
        // enough: the marker buffer is set up once and read_header
        // re-runs cheap-and-clean for each attempt.
        decoder.reset();
        decode_status header_err = decode_status::OK;
        std::size_t  retry_search_start = plan.start_bit;
        // Unbounded retry — terminates only when either a valid block
        // is found OR the blockfinder runs out of candidates within
        // [retry_search_start, plan.stop_at_bit). The chunk's range
        // is bounded so termination is guaranteed.
        while (true) {
            reader.seek_to_bit(retry_search_start);
            header_err = decoder.read_header(reader);
            if (header_err == decode_status::OK) break;
            bit_reader probe{compressed_view};
            probe.seek_to_bit(retry_search_start + 1);
            const std::size_t next_candidate =
                seek_dynamic_huffman_candidate(probe, plan.stop_at_bit);
            if (next_candidate == kBlockfinderSentinel) {
                // No candidate left in the chunk's window — the chunk
                // is effectively empty. Return success with zero bytes;
                // the next chunk's worker will pick up the slack.
                out_chunk.encoded_end_bit_offset = plan.stop_at_bit;
                return decode_status::OK;
            }
            retry_search_start = next_candidate;
        }
        out_chunk.encoded_start_bit_offset = retry_search_start;
    }

    while (true) {
        if (decoder.is_end_of_block()) {
            // Capture the bit position BEFORE reading the next block's
            // header. This is the position where the new block starts,
            // i.e. what the next chunk's blockfinder candidate aligns
            // with.
            const auto next_block_bit = reader.bit_position();
            const auto err = decoder.read_header(reader);
            if (err != decode_status::OK) {
                return err;
            }
            // Preemptive stop (librapidarchive `GzipChunk.hpp:550-554`):
            // stop BEFORE decoding the just-read block when (a) the
            // new block sits at or past `stop_at_bit`, (b) it isn't the
            // member's last block, and (c) it's not a fixed block.
            // Fixed blocks are excluded because the blockfinder doesn't
            // find them as candidates — if we stopped on a fixed block,
            // the next chunk's start_bit (= next dynamic) wouldn't
            // match our stop position and the gap would be lost. So
            // workers naturally chew through any intervening fixed
            // blocks until the next dynamic boundary.
            if (next_block_bit >= plan.stop_at_bit
                && !decoder.is_last_block()
                && decoder.compression_type()
                       != block_compression::FIXED_HUFFMAN) {
                break;
            }
        }

        const auto [view, status] =
            decoder.read(reader, std::numeric_limits<std::size_t>::max());
        if (view.size() > 0) {
            if (view.contains_markers()) {
                out_chunk.uses_marker_storage = true;
                const std::size_t copied =
                    copy_marker_view(out_chunk.marker_storage, view);
                if (copied == std::numeric_limits<std::size_t>::max()) {
                    return decode_status::GENERIC_ERROR;
                }
                out_chunk.decompressed_size_bytes += copied;
            } else if (out_chunk.uses_marker_storage) {
                // Mid-chunk auto-demotion (deflate.cppm calls
                // `set_initial_window({})` once
                // `_distance_to_last_marker_byte` exceeds the live
                // buffer). The DECODED bytes coming out of the
                // decoder are now resolved (no markers in them), but
                // the markers we ALREADY emitted to `marker_storage`
                // are still unresolved — they reference the as-yet-
                // unknown prior chunk. We must keep them for the drain
                // to resolve. Solution: append the direct bytes to
                // `marker_storage` as fake-markers (uint16 with high
                // byte = 0). Drain's resolver leaves these alone (they
                // map to themselves) and only resolves the true
                // markers. Cost: 2× memory for the suffix; acceptable
                // since it's bounded by chunk size and only triggers
                // on chunks with mixed marker+direct output.
                const std::size_t total = view.data_size();
                auto extend_or = out_chunk.marker_storage.extend(total);
                if (!extend_or) {
                    return decode_status::GENERIC_ERROR;
                }
                std::uint16_t* dst = extend_or->data();
                std::size_t dst_pos = 0;
                for (const auto& part : view.data) {
                    for (std::size_t i = 0; i < part.size(); ++i) {
                        dst[dst_pos++] =
                            static_cast<std::uint16_t>(part[i]);
                    }
                }
                out_chunk.decompressed_size_bytes += total;
            } else {
                // Pure direct mode (decoder ran in direct mode from
                // the start, or all-literal chunk that never emitted
                // any marker). Append to byte_storage.
                const std::size_t copied =
                    copy_direct_view(out_chunk.byte_storage, view);
                if (copied == std::numeric_limits<std::size_t>::max()) {
                    return decode_status::GENERIC_ERROR;
                }
                out_chunk.decompressed_size_bytes += copied;
            }
        }
        if (status != decode_status::OK
            && status != decode_status::END_OF_BLOCK
            && status != decode_status::END_OF_STREAM) {
            return status;
        }
        if (decoder.is_end_of_block() && decoder.is_last_block()) {
            // BFINAL=1 — end of member. Worker stops; the gzip
            // footer is consumed by the next-member's header parse
            // (already done at pre-scan time).
            out_chunk.is_member_final = true;
            break;
        }
        // Defence against unbounded output: a false-positive header
        // can build malformed Huffman tables and decode for far longer
        // than expected. Bound the damage to `kMaxChunkDecompressedBytes`.
        if (out_chunk.decompressed_size_bytes
                >= kMaxChunkDecompressedBytes) [[unlikely]] {
            return decode_status::GENERIC_ERROR;
        }
    }

    out_chunk.encoded_end_bit_offset = reader.bit_position();

    if (!out_chunk.uses_marker_storage) {
        const std::size_t window_bytes = std::min<std::size_t>(
            out_chunk.byte_storage.size, kChunkTrailingWindowBytes);
        if (window_bytes > 0u) {
            const std::uint8_t* src =
                out_chunk.byte_storage.data.get()
                + (out_chunk.byte_storage.size - window_bytes);
            std::memcpy(out_chunk.trailing_window.data(), src, window_bytes);
        }
        out_chunk.trailing_window_size = window_bytes;
    }
    return decode_status::OK;
}

}  // namespace detail

// =====================================================================
// chunk_fetcher
// =====================================================================

class chunk_fetcher {
public:
    chunk_fetcher(std::span<const std::uint8_t> compressed_view,
                  std::size_t                   chunk_decompressed_target_bytes,
                  std::size_t                   worker_count,
                  bool                          verify_crc) noexcept
        : _compressed_view(compressed_view),
          _chunk_target_bytes(
              chunk_decompressed_target_bytes == 0
                  ? kDefaultChunkTargetBytes
                  : chunk_decompressed_target_bytes),
          _worker_count(std::clamp(worker_count, std::size_t{1}, kMaxWorkerCount)),
          _verify_crc(verify_crc),
          _slot_lifecycles(kChunkArenaCapacity, detail::slot_lifecycle::FREE),
          _slot_chunk_idx(kChunkArenaCapacity,
                          std::numeric_limits<std::size_t>::max()) {
        _chunk_arena.reserve(kChunkArenaCapacity);
        for (std::size_t i = 0; i < kChunkArenaCapacity; ++i) {
            _chunk_arena.emplace_back(std::make_unique<chunk_data>());
        }
    }

    ~chunk_fetcher() {
        shutdown();
    }

    chunk_fetcher(const chunk_fetcher&) = delete;
    chunk_fetcher& operator=(const chunk_fetcher&) = delete;
    chunk_fetcher(chunk_fetcher&&) = delete;
    chunk_fetcher& operator=(chunk_fetcher&&) = delete;

    void start_producer() noexcept {
        if (_producer_started) return;
        _producer_started = true;
        _producer = std::thread([this]() noexcept {
            if (_worker_count <= 1) {
                producer_loop_single_thread();
            } else {
                producer_loop_parallel();
            }
        });
    }

    [[nodiscard]] std::pair<const chunk_data*, bool>
    get_next_chunk() noexcept {
        start_producer();
        std::unique_lock<std::mutex> guard(_state_mutex);
        // Idx-based wait: consumer reads chunks in chunk_idx order
        // regardless of which slot they live in. Workers and the drain
        // mark slots READY_FOR_CONSUMER as soon as their chunk's
        // resolution finishes — possibly out of idx order, since
        // phase-2 across workers is parallel. The consumer just waits
        // for ITS specific next idx to land in its slot.
        const std::size_t slot = _consumer_next_idx % kChunkArenaCapacity;
        _state_cv.wait(guard, [this, slot]() noexcept {
            return (_slot_lifecycles[slot]
                        == detail::slot_lifecycle::READY_FOR_CONSUMER
                    && _slot_chunk_idx[slot] == _consumer_next_idx)
                || _producer_done.load(std::memory_order_acquire);
        });
        const bool slot_ready =
            _slot_lifecycles[slot]
                == detail::slot_lifecycle::READY_FOR_CONSUMER
            && _slot_chunk_idx[slot] == _consumer_next_idx;
        if (!slot_ready) {
            const bool has_error = !_last_error_message.empty();
            return {nullptr, has_error};
        }
        _slot_lifecycles[slot] = detail::slot_lifecycle::CONSUMER_HOLDING;
        ++_consumer_next_idx;
        _state_cv.notify_all();
        chunk_data* chunk = _chunk_arena[slot].get();
        return {chunk, chunk->state == chunk_state::DECODE_ERROR};
    }

    void release_chunk(const chunk_data* chunk) noexcept {
        if (chunk == nullptr) return;
        std::size_t slot = kChunkArenaCapacity;
        for (std::size_t i = 0; i < kChunkArenaCapacity; ++i) {
            if (_chunk_arena[i].get() == chunk) { slot = i; break; }
        }
        if (slot >= kChunkArenaCapacity) return;
        std::lock_guard<std::mutex> guard(_state_mutex);
        _slot_lifecycles[slot] = detail::slot_lifecycle::FREE;
        _slot_chunk_idx[slot] = std::numeric_limits<std::size_t>::max();
        _state_cv.notify_all();
    }

    [[nodiscard]] std::string last_error_message() const noexcept {
        std::lock_guard<std::mutex> guard(_state_mutex);
        return _last_error_message;
    }

    void release_until(std::size_t /*encoded_bit_offset*/) noexcept {}

    void shutdown() noexcept {
        if (!_producer_started) return;
        _shutdown.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> guard(_state_mutex);
            _state_cv.notify_all();
        }
        if (_producer.joinable()) _producer.join();
        _producer_started = false;
    }

private:
    // -----------------------------------------------------------------
    // Producer (single-thread fallback)
    // -----------------------------------------------------------------

    /// Single-thread per-member loop. Used when `worker_count == 1`.
    /// Each chunk transitions FREE → WORKER_FILLING → READY_FOR_CONSUMER
    /// directly (no intermediate drain step).
    void producer_loop_single_thread() noexcept {
        bit_reader reader{_compressed_view};
        std::size_t fill_cursor = 0;  // round-robin slot index
        std::size_t chunk_idx_counter = 0;  // strict chunk_idx for the
                                            // idx-based consumer.

        while (true) {
            auto header_or = parse_gzip_header(reader, _compressed_view);
            if (!header_or) {
                set_error("gzip header parse failed: " + header_or.error());
                signal_done();
                return;
            }
            block decoder;
            decoder.reset();
            decoder.set_initial_window(std::span<const std::uint8_t>{});

            while (!_shutdown.load(std::memory_order_acquire)) {
                const std::size_t slot = fill_cursor;
                const std::size_t this_chunk_idx = chunk_idx_counter;

                {
                    std::unique_lock<std::mutex> guard(_state_mutex);
                    _state_cv.wait(guard, [this, slot]() noexcept {
                        return _shutdown.load(std::memory_order_acquire)
                            || _slot_lifecycles[slot]
                                   == detail::slot_lifecycle::FREE;
                    });
                    if (_shutdown.load(std::memory_order_acquire)) return;
                    _slot_lifecycles[slot] =
                        detail::slot_lifecycle::WORKER_FILLING;
                    _slot_chunk_idx[slot] = this_chunk_idx;
                }
                fill_cursor = (fill_cursor + 1) % kChunkArenaCapacity;
                ++chunk_idx_counter;

                chunk_data& chunk = *_chunk_arena[slot];
                chunk.encoded_start_bit_offset = reader.bit_position();
                chunk.state = chunk_state::DIRECTLY_DECODED;
                chunk.uses_marker_storage = false;
                chunk.byte_storage.clear();
                chunk.marker_storage.clear();
                chunk.is_member_final = false;
                chunk.decompressed_size_bytes = 0;
                chunk.error_message.assign(std::string_view{});

                if (auto reserve_or =
                        chunk.byte_storage.reserve(_chunk_target_bytes);
                    !reserve_or) {
                    chunk.state = chunk_state::DECODE_ERROR;
                    chunk.error_message.assign(reserve_or.error());
                    set_error(reserve_or.error());
                    publish_to_consumer(slot);
                    signal_done();
                    return;
                }

                const auto [produced, status] =
                    detail::decode_until(decoder, reader, chunk,
                                         _chunk_target_bytes);
                chunk.decompressed_size_bytes = produced;
                chunk.encoded_end_bit_offset = reader.bit_position();
                if (status != decode_status::OK
                    && status != decode_status::END_OF_BLOCK
                    && status != decode_status::END_OF_STREAM) {
                    chunk.state = chunk_state::DECODE_ERROR;
                    chunk.error_message.assign(decode_status_name(status));
                    set_error(std::string{decode_status_name(status)});
                    publish_to_consumer(slot);
                    signal_done();
                    return;
                }

                if (!chunk.uses_marker_storage) {
                    const std::size_t window_bytes = std::min<std::size_t>(
                        chunk.byte_storage.size, kChunkTrailingWindowBytes);
                    if (window_bytes > 0) {
                        const std::uint8_t* src =
                            chunk.byte_storage.data.get()
                            + (chunk.byte_storage.size - window_bytes);
                        std::memcpy(chunk.trailing_window.data(), src, window_bytes);
                    }
                    chunk.trailing_window_size = window_bytes;
                }

                const bool stream_end =
                    decoder.is_end_of_block() && decoder.is_last_block();
                if (stream_end) {
                    chunk.is_member_final = true;
                    publish_to_consumer(slot);
                    reader.align_to_byte_boundary();
                    if (reader.bits_buffered() < 64) reader.refill();
                    if (reader.bits_buffered() >= 64) {
                        (void)reader.read_aligned_le_u64();
                    }
                    break;
                }
                publish_to_consumer(slot);
            }

            const std::size_t consumed_bytes = reader.bit_position() / 8u;
            if (consumed_bytes >= _compressed_view.size()) break;
        }

        signal_done();
    }

    // -----------------------------------------------------------------
    // Producer (parallel-pool path)
    // -----------------------------------------------------------------

    void producer_loop_parallel() noexcept {
        auto members_or = detail::prescan_members(_compressed_view);
        if (!members_or) {
            set_error(members_or.error());
            signal_done();
            return;
        }
        auto plans_or = detail::plan_chunks(
            _compressed_view, *members_or, kDefaultChunkTargetBytes);
        if (!plans_or) {
            set_error(plans_or.error());
            signal_done();
            return;
        }
        std::vector<detail::chunk_plan> plans = std::move(*plans_or);

        const std::size_t total_chunks = plans.size();
        if (total_chunks == 0) {
            signal_done();
            return;
        }

        _next_worker_chunk_idx.store(0, std::memory_order_relaxed);

        // Spawn workers.
        std::vector<std::thread> workers;
        workers.reserve(_worker_count);
        for (std::size_t w = 0; w < _worker_count; ++w) {
            workers.emplace_back([this, &plans, total_chunks]() noexcept {
                worker_loop(plans, total_chunks);
            });
        }

        // Drain in idx order, resolve markers, publish to consumer.
        std::array<std::uint8_t, kChunkTrailingWindowBytes> drain_window{};
        std::size_t drain_window_size = 0;
        std::uint32_t current_member_idx = 0;
        bool fatal_error = false;

        for (std::size_t k = 0; k < total_chunks
             && !_shutdown.load(std::memory_order_acquire)
             && !fatal_error; ++k) {
            const std::size_t slot = k % kChunkArenaCapacity;

            // Wait for slot[k % K] to be filled by the worker that owns
            // chunk_idx == k.
            {
                std::unique_lock<std::mutex> guard(_state_mutex);
                _state_cv.wait(guard, [this, slot, k]() noexcept {
                    return _shutdown.load(std::memory_order_acquire)
                        || (_slot_lifecycles[slot]
                                == detail::slot_lifecycle::READY_FOR_DRAIN
                            && _slot_chunk_idx[slot] == k);
                });
                if (_shutdown.load(std::memory_order_acquire)) break;
            }

            chunk_data& chunk = *_chunk_arena[slot];
            const auto& plan = plans[k];

            // Member-boundary reset.
            if (plan.member_idx != current_member_idx) {
                drain_window_size = 0;
                current_member_idx = plan.member_idx;
            }

            // Worker reported an error?
            if (chunk.state == chunk_state::DECODE_ERROR) {
                set_error(std::string{chunk.error_message.view()});
                publish_to_consumer(slot);
                fatal_error = true;
                break;
            }

            // Phase-1 marker resolution: the last 32 KiB only. Drain
            // runs this serially so the trailing window propagates
            // forward correctly. Phase 2 (the chunk's REMAINDER) is
            // dispatched to the decode-worker pool, which picks it
            // up in parallel after we mark the slot PHASE_1_DONE.
            if (chunk.uses_marker_storage) {
                if (!detail::resolve_markers_phase_1(
                        chunk, drain_window.data(), drain_window_size)) {
                    chunk.state = chunk_state::DECODE_ERROR;
                    chunk.error_message.assign(
                        std::string_view{"resolve_markers: alloc failure"});
                    set_error("resolve_markers: alloc failure");
                    publish_to_consumer(slot);
                    fatal_error = true;
                    break;
                }
            }

            // Update drain_window from the chunk's trailing window.
            const std::size_t chunk_tail = chunk.trailing_window_size;
            if (chunk_tail >= kChunkTrailingWindowBytes) {
                std::memcpy(drain_window.data(),
                            chunk.trailing_window.data(),
                            kChunkTrailingWindowBytes);
                drain_window_size = kChunkTrailingWindowBytes;
            } else if (chunk_tail > 0u) {
                const std::size_t keep =
                    std::min<std::size_t>(
                        drain_window_size,
                        kChunkTrailingWindowBytes - chunk_tail);
                if (keep > 0u) {
                    std::array<std::uint8_t, kChunkTrailingWindowBytes>
                        scratch{};
                    std::memcpy(scratch.data(),
                                drain_window.data() + (drain_window_size - keep),
                                keep);
                    std::memcpy(drain_window.data(), scratch.data(), keep);
                }
                std::memcpy(drain_window.data() + keep,
                            chunk.trailing_window.data(),
                            chunk_tail);
                drain_window_size =
                    std::min<std::size_t>(
                        keep + chunk_tail, kChunkTrailingWindowBytes);
            }

            if (chunk.is_member_final) {
                drain_window_size = 0;
            }

            chunk.state = chunk_state::DIRECTLY_DECODED;
            if (chunk.uses_marker_storage) {
                // Hand off to a phase-2 worker. After phase 2 finishes,
                // the worker will mark READY_FOR_CONSUMER itself.
                std::lock_guard<std::mutex> guard(_state_mutex);
                _slot_lifecycles[slot] =
                    detail::slot_lifecycle::PHASE_1_DONE;
                _state_cv.notify_all();
            } else {
                // Direct-mode chunk — already fully resolved by the
                // worker. Skip phase 2; publish straight to consumer.
                publish_to_consumer(slot);
            }
        }

        // Tell workers to stop.
        _shutdown.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> guard(_state_mutex);
            _state_cv.notify_all();
        }
        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }

        signal_done();
    }

    /// One worker thread. Claims chunks atomically, decodes them into
    /// the arena slot at `chunk_idx % K`, signals drain.
    void worker_loop(const std::vector<detail::chunk_plan>& plans,
                     std::size_t                            total_chunks) noexcept {
        // Heap-allocate the decoder — its 128 KiB `_window16` is too
        // big for the worker's stack on macOS.
        auto decoder = std::make_unique<block>();

        while (true) {
            const std::size_t chunk_idx =
                _next_worker_chunk_idx.fetch_add(1u,
                                                 std::memory_order_relaxed);
            if (chunk_idx >= total_chunks) break;
            if (_shutdown.load(std::memory_order_acquire)) break;

            const std::size_t slot = chunk_idx % kChunkArenaCapacity;

            // Wait for slot to be FREE (consumer released previous
            // chunk).
            {
                std::unique_lock<std::mutex> guard(_state_mutex);
                _state_cv.wait(guard, [this, slot]() noexcept {
                    return _shutdown.load(std::memory_order_acquire)
                        || _slot_lifecycles[slot]
                               == detail::slot_lifecycle::FREE;
                });
                if (_shutdown.load(std::memory_order_acquire)) break;
                _slot_lifecycles[slot] =
                    detail::slot_lifecycle::WORKER_FILLING;
                _slot_chunk_idx[slot] = chunk_idx;
            }

            chunk_data& chunk = *_chunk_arena[slot];
            const auto err = detail::worker_decode_chunk(
                *decoder, _compressed_view, plans[chunk_idx], chunk);
            if (err != decode_status::OK) {
                chunk.state = chunk_state::DECODE_ERROR;
                chunk.error_message.assign(decode_status_name(err));
            } else {
                chunk.state = chunk_state::DIRECTLY_DECODED;
            }

            const bool needs_phase_2 = chunk.uses_marker_storage
                && err == decode_status::OK;

            {
                std::lock_guard<std::mutex> guard(_state_mutex);
                _slot_lifecycles[slot] =
                    detail::slot_lifecycle::READY_FOR_DRAIN;
                _state_cv.notify_all();
            }

            // Phase 2: if this chunk needs marker resolution, wait for
            // the drain to finish phase 1 (which propagates the
            // trailing window through the chain). Then resolve the
            // chunk's REMAINDER (everything except the last 32 KiB
            // already resolved by drain) using the prior_window
            // snapshot the drain saved on this chunk. Phase 2 is
            // independent across chunks — multiple workers run it
            // concurrently while the drain marches forward sequentially.
            //
            // Shutdown vs phase-1-done race: drain sets `_shutdown =
            // true` IMMEDIATELY after marking PHASE_1_DONE on the last
            // chunk and breaking its loop. A worker that's just woken
            // up sees BOTH predicates true. The check below has to run
            // phase 2 anyway when PHASE_1_DONE was set on this slot —
            // otherwise the last chunk's prefix (everything before the
            // tail 32 KiB) never gets resolved and the consumer drops
            // its bytes silently. Only break-without-phase-2 when
            // shutdown was set BEFORE the drain reached this chunk
            // (i.e. PHASE_1_DONE not set on this slot/idx).
            if (needs_phase_2) {
                bool phase_1_ready = false;
                {
                    std::unique_lock<std::mutex> guard(_state_mutex);
                    _state_cv.wait(guard, [this, slot, chunk_idx]() noexcept {
                        return _shutdown.load(std::memory_order_acquire)
                            || (_slot_lifecycles[slot]
                                    == detail::slot_lifecycle::PHASE_1_DONE
                                && _slot_chunk_idx[slot] == chunk_idx);
                    });
                    phase_1_ready =
                        _slot_lifecycles[slot]
                            == detail::slot_lifecycle::PHASE_1_DONE
                        && _slot_chunk_idx[slot] == chunk_idx;
                }
                if (!phase_1_ready) break;
                detail::resolve_markers_phase_2(chunk);
                publish_to_consumer(slot);
            }
        }
    }

    // -----------------------------------------------------------------
    // Shared producer helpers
    // -----------------------------------------------------------------

    /// Publish a slot to the consumer. Slot's chunk is fully resolved.
    /// Consumer reads in chunk_idx order via the idx-based wait in
    /// `get_next_chunk` — slots can be marked READY_FOR_CONSUMER out
    /// of order (parallel phase-2) and the consumer still walks the
    /// idx sequence linearly.
    void publish_to_consumer(std::size_t slot) noexcept {
        std::lock_guard<std::mutex> guard(_state_mutex);
        _slot_lifecycles[slot] =
            detail::slot_lifecycle::READY_FOR_CONSUMER;
        _state_cv.notify_all();
    }

    void signal_done() noexcept {
        _producer_done.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> guard(_state_mutex);
        _state_cv.notify_all();
    }

    void set_error(std::string message) noexcept {
        std::lock_guard<std::mutex> guard(_state_mutex);
        if (_last_error_message.empty()) {
            _last_error_message = std::move(message);
        }
    }

    // -----------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------

    std::span<const std::uint8_t> _compressed_view;
    std::size_t                   _chunk_target_bytes;
    std::size_t                   _worker_count;
    bool                          _verify_crc;

    std::vector<std::unique_ptr<chunk_data>> _chunk_arena;

    mutable std::mutex             _state_mutex;
    std::condition_variable        _state_cv;

    /// Per-slot lifecycle. Source of truth for slot ownership.
    std::vector<detail::slot_lifecycle> _slot_lifecycles;
    /// chunk_idx currently in slot[i] (max_size_t = unset). Set by
    /// workers, cleared on consumer release.
    std::vector<std::size_t>            _slot_chunk_idx;

    // Consumer's next-to-read chunk_idx. Strictly monotonic.
    std::size_t                    _consumer_next_idx = 0;

    // Parallel-path coordination.
    std::atomic<std::size_t>       _next_worker_chunk_idx{0};

    std::atomic<bool>              _shutdown{false};
    std::atomic<bool>              _producer_done{false};
    std::thread                    _producer;
    bool                           _producer_started = false;
    std::string                    _last_error_message;
};

}  // namespace bioseq::io::compression
