/**
 * @file chunk_fetcher.cppm
 * @brief Plans, decodes, and drains gzip chunks in encoded order.
 *
 * The single-worker path decodes members in order. The parallel path first
 * discovers member boundaries and independent dynamic-Huffman block starts,
 * then submits bounded decode work to the shared task pool. An ordered drain
 * propagates the 32 KiB dependency window before publishing chunks.
 *
 * Every arena lifecycle transition holds `_state_mutex` and notifies
 * `_state_cv`. A monotonically increasing chunk tag prevents a recycled slot
 * from being consumed for the wrong decode request.
 */
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.io.compression.chunk_fetcher;

import bioseq.io.simd;
import bioseq.io.compression.aligned_byte_buffer;
import bioseq.io.compression.bit_reader;
import bioseq.io.compression.block_finder;
import bioseq.io.compression.chunk;
import bioseq.io.compression.deflate;
import bioseq.io.compression.gzip_format;
import bioseq.io.compression.huffman;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;

export namespace bioseq::io::compression {

/// Maximum number of decoded chunks retained between workers and consumer.
inline constexpr std::size_t chunk_arena_capacity = 8;

/// Default compressed-byte target used when the caller supplies zero.
inline constexpr std::size_t default_chunk_target_bytes = 32 * 1024 * 1024;

/// Upper bound on concurrent decoders sharing the fixed-capacity arena.
inline constexpr std::size_t maximum_worker_count = 12;

/// Hard cap on one chunk's decoded output. Exceeding it returns an error.
inline constexpr std::size_t maximum_chunk_decompressed_bytes = 256 * 1024 * 1024;

namespace detail {

// =====================================================================
// Slot lifecycle (parallel path)
// =====================================================================

/// Ownership state of one tagged chunk-arena slot.
enum class slot_lifecycle : std::uint8_t {
    free = 0,
    worker_filling = 1,
    ready_for_drain = 2,
    trailing_window_resolved = 3,
    ready_for_consumer = 4,
    consumer_holding = 5,
};

// =====================================================================
// Member pre-scan
// =====================================================================

/// Header boundary required to plan one gzip member.
struct gzip_member_descriptor {
    /// Byte offset of the gzip-magic byte (0x1f).
    std::size_t header_byte = 0;
    /// First bit of the first deflate block in this member (= header
    /// end bit).
    std::size_t header_end_bit = 0;
};

/// Lightweight validation of a dynamic-Huffman block header at the
/// reader's current position. Mirrors `deflate_block_decoder::read_header`
/// but skips the literal/distance Huffman table builds and instead
/// runs the Kraft completeness check on each table's code lengths.
[[nodiscard]] inline bool validate_dynamic_huffman_only(bit_reader& reader) noexcept;

/// Walk `compressed_view` linearly looking for gzip-magic candidates
/// (`0x1f 0x8b 0x08 <flags ≤ 0x1f>`). Each candidate is fed through:
///
///   1. `parse_gzip_header` — magic bytes, method, NUL-terminated
///      FNAME / FCOMMENT (when flagged).
///   2. `validate_dynamic_huffman_only` at the would-be first deflate
///      block — Kraft completeness check on the literal+distance
///      trees.
///
/// The Huffman validation rejects magic-byte sequences embedded in compressed
/// data before they become member boundaries. Parallel planning accepts only
/// members whose first DEFLATE block is dynamic-Huffman. Other layouts require
/// the single-worker producer.
[[nodiscard]] inline std::expected<std::vector<gzip_member_descriptor>, std::string>
prescan_members(std::span<const std::uint8_t> compressed_view) noexcept {
    std::vector<gzip_member_descriptor> members;
    if (compressed_view.empty())
        return members;

    std::size_t cursor = 0;
    while (cursor + 4 <= compressed_view.size()) {
        // SIMD-scan for the next 0x1f candidate.
        const std::size_t advance = bioseq::io::simd::scan_first_byte(
            std::uint8_t{0x1F}, compressed_view.data() + cursor, compressed_view.size() - cursor);
        cursor += advance;
        if (cursor + 4 > compressed_view.size())
            break;

        // Cheap pre-filter: 0x1f 0x8b 0x08 <flags ≤ 0x1f>.
        if (compressed_view[cursor + 0] != 0x1F || compressed_view[cursor + 1] != 0x8B ||
            compressed_view[cursor + 2] != 0x08 || compressed_view[cursor + 3] > 0x1F) {
            ++cursor;
            continue;
        }

        // Step 1: parse the header itself (magic + flags + optional
        // NUL-terminated fields).
        bit_reader probe{std::span<const std::uint8_t>{compressed_view.data() + cursor,
                                                       compressed_view.size() - cursor}};
        auto header_or =
            parse_gzip_header(probe,
                              std::span<const std::uint8_t>{compressed_view.data() + cursor,
                                                            compressed_view.size() - cursor});
        if (!header_or) {
            ++cursor;
            continue;
        }
        const std::size_t header_end_bit_global = cursor * 8u + header_or->end_bit_offset;

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

        gzip_member_descriptor info;
        info.header_byte = cursor;
        info.header_end_bit = header_end_bit_global;
        members.push_back(info);

        // Skip past the header. The deflate stream starts here.
        // we'll rediscover the next member when we hit its magic.
        cursor = info.header_end_bit / 8u;
        if ((info.header_end_bit % 8u) != 0u)
            ++cursor;
    }

    if (members.empty()) {
        return std::unexpected(std::string{"prescan_members: no gzip member detected in input"});
    }
    return members;
}

// =====================================================================
// Chunk planning
// =====================================================================

/// Encoded interval and member position assigned to one chunk worker.
struct chunk_decode_plan {
    std::uint32_t member_index = 0;
    /// Bit offset of a valid deflate-block start. Worker seeks here.
    std::size_t start_bit = 0;
    /// Worker stops decoding once a block starts at-or-after this bit
    /// without decoding a block owned by the next plan.
    std::size_t stop_at_bit = 0;
    /// True for the very first chunk of the member (decoder runs in
    /// direct mode with an empty window).
    bool is_first_in_member = false;
    /// True for the chunk that contains the BFINAL=1 block of the
    /// member. Drain resets `_drain_window_size` after it.
    bool is_last_in_member = false;
};

/// Validate a dynamic-Huffman block at the reader's current bit position.
[[nodiscard]] inline bool validate_dynamic_huffman_only(bit_reader& reader) noexcept {
    if (reader.bits_buffered() < 3)
        reader.refill();
    if (reader.bits_buffered() < 3)
        return false;
    const std::uint64_t bfinal_btype = reader.read<3>();
    const std::uint64_t btype = (bfinal_btype >> 1) & 0x3u;
    if (btype != 0x2u)
        return false;

    if (reader.bits_buffered() < 14)
        reader.refill();
    if (reader.bits_buffered() < 14)
        return false;
    const std::size_t literal_count = static_cast<std::size_t>(reader.read<5>()) + 257u;
    if (literal_count > maximum_literal_or_length_symbols)
        return false;
    const std::size_t distance_count = static_cast<std::size_t>(reader.read<5>()) + 1u;
    if (distance_count > maximum_distance_symbol_count)
        return false;
    const std::size_t precode_count = static_cast<std::size_t>(reader.read<4>()) + 4u;

    std::array<std::uint8_t, maximum_precode_count> precode_cl{};
    for (std::size_t i = 0; i < precode_count; ++i) {
        if (reader.bits_buffered() < precode_bit_count)
            reader.refill();
        // Fully-qualify `precode_alphabet` — the unqualified name
        // shadows on `bioseq::io::compression::detail::precode_alphabet`
        // (from huffman.cppm), which is a size_t count, not the
        // alphabet array (which lives in the parent namespace).
        precode_cl[::bioseq::io::compression::precode_alphabet[i]] =
            static_cast<std::uint8_t>(reader.read<precode_bit_count>());
    }

    const auto precode =
        precode_decoder::build(std::span<const std::uint8_t>{precode_cl.data(), precode_cl.size()});
    if (!precode.is_valid())
        return false;

    std::array<std::uint8_t, literal_and_distance_code_length_buffer_size> literal_cl{};
    if (auto err = read_distance_and_literal_code_lengths(
            literal_cl, reader, precode, literal_count + distance_count);
        err != decode_status::ok) {
        return false;
    }
    if (literal_cl[end_of_block_symbol] == 0)
        return false;
    if (!lit_len_decoder::is_valid_code_lengths(
            std::span<const std::uint8_t>{literal_cl.data(), literal_count})) {
        return false;
    }
    if (!distance_decoder::is_valid_code_lengths(
            std::span<const std::uint8_t>{literal_cl.data() + literal_count, distance_count})) {
        return false;
    }
    return true;
}

/// Build a per-member chunk plan. For each member, the first chunk
/// starts at the header-end bit. Subsequent chunks find a valid
/// dynamic-Huffman block via `seek_dynamic_huffman_candidate`. The
/// `stop_at_bit` of chunk K is the `start_bit` of chunk K+1 (or, for
/// the last chunk of a member, the next member's start or end of file).
///
/// Every noninitial `start_bit` passes full literal and distance Kraft
/// validation. Precode-only candidates are insufficient as chunk boundaries.
[[nodiscard]] inline std::expected<std::vector<chunk_decode_plan>, std::string>
plan_chunks(std::span<const std::uint8_t> compressed_view,
            const std::vector<gzip_member_descriptor>& members,
            std::size_t chunk_target_compressed_bytes) noexcept {
    std::vector<chunk_decode_plan> plans;
    if (members.empty()) {
        return std::unexpected(std::string{"plan_chunks: no members"});
    }

    const std::size_t total_bits = compressed_view.size() * 8u;

    // Skip false-positive headers until a complete dynamic-Huffman block validates.
    auto find_validated_block = [&compressed_view](std::size_t start_bit,
                                                   std::size_t until_bit) noexcept -> std::size_t {
        std::size_t cursor = start_bit;
        while (cursor < until_bit) {
            bit_reader scan{compressed_view};
            scan.seek_to_bit(cursor);
            const std::size_t candidate = seek_dynamic_huffman_candidate(scan, until_bit);
            if (candidate == block_finder_sentinel) {
                return block_finder_sentinel;
            }
            bit_reader probe{compressed_view};
            probe.seek_to_bit(candidate);
            // Validate both code-length sets without constructing the
            // literal and distance lookup tables.
            if (validate_dynamic_huffman_only(probe)) {
                return candidate;
            }
            cursor = candidate + 1u;
        }
        return block_finder_sentinel;
    };

    // Bound candidate validation to the same pool used for decoding.
    const unsigned hw_threads = bioseq::parallelism::default_concurrency_limit();
    const std::size_t plan_workers = std::min<std::size_t>(hw_threads > 2 ? hw_threads - 2 : 1, 8);

    for (std::size_t m = 0; m < members.size(); ++m) {
        const std::size_t member_start_bit = members[m].header_end_bit;
        const std::size_t member_end_bit =
            (m + 1u < members.size()) ? (members[m + 1u].header_byte * 8u) : total_bits;
        if (member_end_bit <= member_start_bit) {
            return std::unexpected(std::string{"plan_chunks: member spans zero bytes"});
        }
        const std::size_t member_compressed_bits = member_end_bit - member_start_bit;
        const std::size_t member_compressed_bytes = member_compressed_bits / 8u;

        const std::size_t target_bytes = chunk_target_compressed_bytes == 0
                                             ? default_chunk_target_bytes
                                             : chunk_target_compressed_bytes;
        std::size_t n_chunks = member_compressed_bytes / target_bytes;
        if (n_chunks == 0u)
            n_chunks = 1u;
        if (n_chunks > 4096u)
            n_chunks = 4096u;

        std::vector<std::size_t> plan_start_bits;
        plan_start_bits.reserve(n_chunks);
        plan_start_bits.push_back(member_start_bit);

        // Validate chunk start bits in parallel. Results are collected
        // by chunk index and merged in encoded order.
        std::vector<std::size_t> per_chunk_start(n_chunks, block_finder_sentinel);
        per_chunk_start[0] = member_start_bit;

        auto& pool = bioseq::parallelism::global_pool::instance();
        if (!pool.is_active()) {
            pool.configure(bioseq::parallelism::pool_config{
                .maximum_concurrency =
                    static_cast<unsigned>(std::max<std::size_t>(1, plan_workers)),
                .mode = bioseq::parallelism::pipeline_concurrency_mode::dispatch_parallel,
            });
        }
        if (n_chunks > 1) {
            pool.parallel_for(
                1,
                n_chunks,
                1,
                bioseq::parallelism::range_body{
                    [&per_chunk_start,
                     &find_validated_block,
                     member_start_bit,
                     member_compressed_bits,
                     member_end_bit,
                     n_chunks](std::size_t lo, std::size_t hi) noexcept {
                        for (std::size_t k = lo; k < hi && k < n_chunks; ++k) {
                            const std::size_t target_bit =
                                member_start_bit + (k * member_compressed_bits) / n_chunks;
                            per_chunk_start[k] = find_validated_block(target_bit, member_end_bit);
                        }
                    }});
        }
        // Merge in chunk-index order, skipping sentinels and duplicates.
        for (std::size_t k = 1; k < n_chunks; ++k) {
            const std::size_t candidate = per_chunk_start[k];
            if (candidate == block_finder_sentinel)
                continue;
            if (!plan_start_bits.empty() && plan_start_bits.back() == candidate)
                continue;
            plan_start_bits.push_back(candidate);
        }

        for (std::size_t k = 0; k < plan_start_bits.size(); ++k) {
            chunk_decode_plan plan;
            plan.member_index = static_cast<std::uint32_t>(m);
            plan.start_bit = plan_start_bits[k];
            plan.stop_at_bit =
                (k + 1u < plan_start_bits.size()) ? plan_start_bits[k + 1u] : member_end_bit;
            plan.is_first_in_member = (k == 0u);
            plan.is_last_in_member = (k + 1u == plan_start_bits.size());
            plans.push_back(plan);
        }
    }

    return plans;
}

// =====================================================================
// Marker resolver
// =====================================================================

/// Resolve `count` marker or literal lanes from
/// `src` into uint8 bytes at `dst`, using `prev_window` for marker
/// lookups.
inline void resolve_lane_range(const std::uint16_t* src,
                               std::uint8_t* dst,
                               std::size_t count,
                               const std::uint8_t* prev_window,
                               std::size_t prev_window_size) noexcept {
    alignas(64) std::array<std::uint8_t, 0x8000> padded_prev{};
    if (prev_window_size > 0u) {
        std::memcpy(padded_prev.data(),
                    prev_window,
                    std::min(prev_window_size, static_cast<std::size_t>(0x8000)));
    }
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint16_t value = src[i];
        const std::uint16_t low15 = value & 0x7FFFu;
        const std::uint8_t marker_byte = padded_prev[low15];
        const std::uint8_t literal_byte = static_cast<std::uint8_t>(value);
        const std::uint8_t is_marker_mask =
            static_cast<std::uint8_t>(-static_cast<std::int8_t>((value >> 15) & 0x1u));
        dst[i] = static_cast<std::uint8_t>((is_marker_mask & marker_byte) |
                                           (~is_marker_mask & literal_byte));
    }
}

/// Resolve the chunk's trailing 32 KiB. This operation is sequential
/// across chunks (each chunk's trailing window depends on the
/// previous chunk's). Saves the prior `drain_window` snapshot into
/// the chunk so workers can resolve the remainder against the same
/// previous window in parallel.
[[nodiscard]] inline bool resolve_trailing_window_markers(decoded_chunk& chunk,
                                                          const std::uint8_t* drain_window,
                                                          std::size_t drain_window_size) noexcept {
    if (!chunk.uses_marker_storage)
        return true;
    const std::size_t marker_count = chunk.marker_storage.size;

    auto reserve_or = chunk.byte_storage.reserve(marker_count);
    if (!reserve_or)
        return false;
    auto extend_or = chunk.byte_storage.extend(marker_count);
    if (!extend_or)
        return false;
    std::uint8_t* dst = extend_or->data();
    const std::uint16_t* src = chunk.marker_storage.data.get();

    chunk.prior_window_snapshot_size = std::min(drain_window_size, chunk_trailing_window_bytes);
    if (chunk.prior_window_snapshot_size > 0) {
        std::memcpy(
            chunk.prior_window_snapshot.data(), drain_window, chunk.prior_window_snapshot_size);
    }

    const std::size_t tail_size = std::min<std::size_t>(marker_count, chunk_trailing_window_bytes);
    const std::size_t tail_begin = marker_count - tail_size;
    resolve_lane_range(
        src + tail_begin, dst + tail_begin, tail_size, drain_window, drain_window_size);

    if (tail_size > 0u) {
        std::memcpy(chunk.trailing_window.data(), dst + tail_begin, tail_size);
    }
    chunk.trailing_window_size = tail_size;
    return true;
}

/// Resolve the chunk prefix not covered by the trailing window. Chunks are
/// independent after the ordered drain has saved `prior_window_snapshot`.
inline void resolve_remaining_markers(decoded_chunk& chunk) noexcept {
    if (!chunk.uses_marker_storage) {
        return;
    }
    const std::size_t marker_count = chunk.marker_storage.size;
    if (marker_count <= chunk_trailing_window_bytes) {
        // The trailing-window pass already resolved the whole chunk.
        chunk.marker_storage.clear();
        chunk.uses_marker_storage = false;
        return;
    }

    const std::size_t prefix_size = marker_count - chunk_trailing_window_bytes;
    std::uint8_t* dst = chunk.byte_storage.data.get();
    const std::uint16_t* src = chunk.marker_storage.data.get();
    resolve_lane_range(src,
                       dst,
                       prefix_size,
                       chunk.prior_window_snapshot.data(),
                       chunk.prior_window_snapshot_size);

    chunk.marker_storage.clear();
    chunk.uses_marker_storage = false;
}

// =====================================================================
// Direct-mode buffer copy helpers
// =====================================================================

/// Copy resolved spans into contiguous byte storage.
[[nodiscard]] inline std::size_t copy_direct_view(chunk_byte_buffer& byte_storage,
                                                  const decoded_data_view& view) noexcept {
    const std::size_t total = view.data_size();
    if (total == 0)
        return 0;
    auto extend_or = byte_storage.extend(total);
    if (!extend_or)
        return std::numeric_limits<std::size_t>::max();
    std::uint8_t* dst = extend_or->data();
    std::size_t copied = 0;
    for (const auto& part : view.data) {
        if (part.empty())
            continue;
        std::memcpy(dst + copied, part.data(), part.size());
        copied += part.size();
    }
    return copied;
}

/// Copy marker spans into contiguous marker storage.
[[nodiscard]] inline std::size_t copy_marker_view(chunk_marker_buffer& marker_storage,
                                                  const decoded_data_view& view) noexcept {
    const std::size_t total = view.data_with_markers_size();
    if (total == 0)
        return 0;
    auto extend_or = marker_storage.extend(total);
    if (!extend_or)
        return std::numeric_limits<std::size_t>::max();
    std::uint16_t* dst = extend_or->data();
    std::size_t copied = 0;
    for (const auto& part : view.data_with_markers) {
        if (part.empty())
            continue;
        std::memcpy(dst + copied, part.data(), part.size() * sizeof(std::uint16_t));
        copied += part.size();
    }
    return copied;
}

/// Drive `deflate_block_decoder` over `reader` until `target_bytes` of decompressed
/// output have been emitted (or end-of-stream). Used by the
/// single-thread producer fallback.
[[nodiscard]] inline std::pair<std::size_t, decode_status>
decode_until(deflate_block_decoder& decoder,
             bit_reader& reader,
             decoded_chunk& out_chunk,
             std::size_t target_bytes) noexcept {
    std::size_t produced = 0;
    decode_status final_status = decode_status::ok;

    while (produced < target_bytes) {
        if (decoder.is_end_of_block()) {
            const auto err = decoder.read_header(reader);
            if (err != decode_status::ok) {
                final_status = err;
                break;
            }
        }
        const std::size_t want = std::min(target_bytes - produced, std::size_t{1024 * 1024});
        const auto [view, status] = decoder.read(reader, want);
        if (view.size() > 0) {
            if (view.contains_markers()) {
                out_chunk.uses_marker_storage = true;
                const std::size_t copied = copy_marker_view(out_chunk.marker_storage, view);
                if (copied == std::numeric_limits<std::size_t>::max()) {
                    final_status = decode_status::generic_error;
                    break;
                }
                produced += copied;
            } else {
                if (out_chunk.uses_marker_storage && out_chunk.marker_storage.size > 0) {
                    auto reserve_or = out_chunk.byte_storage.reserve(out_chunk.marker_storage.size +
                                                                     view.data_size());
                    if (!reserve_or) {
                        final_status = decode_status::generic_error;
                        break;
                    }
                    auto extend_or = out_chunk.byte_storage.extend(out_chunk.marker_storage.size);
                    if (!extend_or) {
                        final_status = decode_status::generic_error;
                        break;
                    }
                    std::uint8_t* dst = extend_or->data();
                    const std::uint16_t* src = out_chunk.marker_storage.data.get();
                    for (std::size_t i = 0; i < out_chunk.marker_storage.size; ++i) {
                        dst[i] = static_cast<std::uint8_t>(src[i] & 0xFFu);
                    }
                    out_chunk.marker_storage.clear();
                    out_chunk.uses_marker_storage = false;
                }
                const std::size_t copied = copy_direct_view(out_chunk.byte_storage, view);
                if (copied == std::numeric_limits<std::size_t>::max()) {
                    final_status = decode_status::generic_error;
                    break;
                }
                produced += copied;
            }
        }
        if (status != decode_status::ok) {
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

/// Decode a single chunk per a `chunk_decode_plan`. Invoked from each worker
/// task. The worker owns a heap-allocated decoder and a
/// fresh `bit_reader` over `compressed_view`.
[[nodiscard]] inline decode_status
worker_decode_chunk(deflate_block_decoder& decoder,
                    std::span<const std::uint8_t> compressed_view,
                    const chunk_decode_plan& plan,
                    decoded_chunk& out_chunk) noexcept {
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
        // `plan.start_bit` was returned by candidate scanning. Retry
        // within this plan if full header decoding rejects it.
        //
        // We deliberately avoid `deflate_block_decoder::reset()` between attempts —
        // its 128 KiB marker-buffer memcpy dominates wall clock at
        // false-positive rates. `read_header` is idempotent on state
        // (it overwrites compression_type, is_last_block, and Huffman
        // tables unconditionally), so a single reset() upfront is
        // enough: the marker buffer is set up once and read_header
        // re-runs cheap-and-clean for each attempt.
        decoder.reset();
        decode_status header_err = decode_status::ok;
        std::size_t retry_search_start = plan.start_bit;
        // Unbounded retry — terminates only when either a valid block
        // is found OR the blockfinder runs out of candidates within
        // [retry_search_start, plan.stop_at_bit). The chunk's range
        // is bounded so termination is guaranteed.
        while (true) {
            reader.seek_to_bit(retry_search_start);
            header_err = decoder.read_header(reader);
            if (header_err == decode_status::ok)
                break;
            bit_reader probe{compressed_view};
            probe.seek_to_bit(retry_search_start + 1);
            const std::size_t next_candidate =
                seek_dynamic_huffman_candidate(probe, plan.stop_at_bit);
            if (next_candidate == block_finder_sentinel) {
                // No candidate left in the chunk's window — the chunk
                // is effectively empty. Return success with zero bytes.
                // the next chunk's worker will pick up the slack.
                out_chunk.encoded_end_bit_offset = plan.stop_at_bit;
                return decode_status::ok;
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
            if (err != decode_status::ok) {
                return err;
            }
            // Stop before decoding the just-read block when (a) the
            // new block sits at or past `stop_at_bit`, (b) it isn't the
            // member's last block, and (c) it's not a fixed block.
            // Fixed blocks are excluded because the blockfinder doesn't
            // find them as candidates — if we stopped on a fixed block,
            // the next chunk's start_bit (= next dynamic) wouldn't
            // match our stop position and the gap would be lost. So
            // workers naturally chew through any intervening fixed
            // blocks until the next dynamic boundary.
            if (next_block_bit >= plan.stop_at_bit && !decoder.is_last_block() &&
                decoder.compression_type() != block_compression::fixed_huffman) {
                break;
            }
        }

        const auto [view, status] = decoder.read(reader, std::numeric_limits<std::size_t>::max());
        if (view.size() > 0) {
            if (view.contains_markers()) {
                out_chunk.uses_marker_storage = true;
                const std::size_t copied = copy_marker_view(out_chunk.marker_storage, view);
                if (copied == std::numeric_limits<std::size_t>::max()) {
                    return decode_status::generic_error;
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
                // markers. The suffix uses twice its byte count in memory.
                // since it's bounded by chunk size and only triggers
                // on chunks with mixed marker+direct output.
                const std::size_t total = view.data_size();
                auto extend_or = out_chunk.marker_storage.extend(total);
                if (!extend_or) {
                    return decode_status::generic_error;
                }
                std::uint16_t* dst = extend_or->data();
                std::size_t dst_pos = 0;
                for (const auto& part : view.data) {
                    for (std::size_t i = 0; i < part.size(); ++i) {
                        dst[dst_pos++] = static_cast<std::uint16_t>(part[i]);
                    }
                }
                out_chunk.decompressed_size_bytes += total;
            } else {
                // Pure direct mode (decoder ran in direct mode from
                // the start, or all-literal chunk that never emitted
                // any marker). Append to byte_storage.
                const std::size_t copied = copy_direct_view(out_chunk.byte_storage, view);
                if (copied == std::numeric_limits<std::size_t>::max()) {
                    return decode_status::generic_error;
                }
                out_chunk.decompressed_size_bytes += copied;
            }
        }
        if (status != decode_status::ok && status != decode_status::end_of_block &&
            status != decode_status::end_of_stream) {
            return status;
        }
        if (decoder.is_end_of_block() && decoder.is_last_block()) {
            // BFINAL=1 ends the member. The worker stops and the gzip
            // footer is consumed by the next-member's header parse
            // (already done at pre-scan time).
            out_chunk.is_member_final = true;
            break;
        }
        // Defence against unbounded output: a false-positive header
        // can build malformed Huffman tables and decode for far longer
        // than expected. Bound the damage to `maximum_chunk_decompressed_bytes`.
        if (out_chunk.decompressed_size_bytes >= maximum_chunk_decompressed_bytes) [[unlikely]] {
            return decode_status::generic_error;
        }
    }

    out_chunk.encoded_end_bit_offset = reader.bit_position();

    if (!out_chunk.uses_marker_storage) {
        const std::size_t window_bytes =
            std::min<std::size_t>(out_chunk.byte_storage.size, chunk_trailing_window_bytes);
        if (window_bytes > 0u) {
            const std::uint8_t* src =
                out_chunk.byte_storage.data.get() + (out_chunk.byte_storage.size - window_bytes);
            std::memcpy(out_chunk.trailing_window.data(), src, window_bytes);
        }
        out_chunk.trailing_window_size = window_bytes;
    }
    return decode_status::ok;
}

} // namespace detail

// =====================================================================
// chunk_fetcher
// =====================================================================

/// Plans, decodes, and exposes gzip chunks in encoded order.
class chunk_fetcher {
  public:
    /// Construct over caller-owned compressed bytes.
    chunk_fetcher(std::span<const std::uint8_t> compressed_view,
                  std::size_t chunk_decompressed_target_bytes,
                  std::size_t worker_count,
                  bool verify_crc) noexcept
        : _compressed_view(compressed_view),
          _chunk_target_bytes(chunk_decompressed_target_bytes == 0
                                  ? default_chunk_target_bytes
                                  : chunk_decompressed_target_bytes),
          _worker_count(std::clamp(worker_count, std::size_t{1}, maximum_worker_count)),
          _verify_crc(verify_crc),
          _slot_lifecycles(chunk_arena_capacity, detail::slot_lifecycle::free),
          _slot_chunk_index(chunk_arena_capacity, std::numeric_limits<std::size_t>::max()) {
        _chunk_arena.reserve(chunk_arena_capacity);
        for (std::size_t i = 0; i < chunk_arena_capacity; ++i) {
            _chunk_arena.emplace_back(std::make_unique<decoded_chunk>());
        }
    }

    /// Stop background work before releasing decoder and arena state.
    ~chunk_fetcher() {
        shutdown();
    }

    /// Fetchers exclusively own their task group and chunk arena.
    chunk_fetcher(const chunk_fetcher&) = delete;

    /// Fetchers cannot copy active producer state into another instance.
    chunk_fetcher& operator=(const chunk_fetcher&) = delete;

    /// Fetchers cannot transfer active producer state between instances.
    chunk_fetcher(chunk_fetcher&&) = delete;

    /// Fetchers cannot replace their task group through move assignment.
    chunk_fetcher& operator=(chunk_fetcher&&) = delete;

    /// Start the background producer once.
    void start_producer() noexcept {
        if (_producer_started)
            return;
        _producer_started = true;
        auto& pool = bioseq::parallelism::global_pool::instance();
        _producer_group = pool.make_task_group();
        _producer_group.run(bioseq::parallelism::task_body{[this]() noexcept {
            if (_worker_count <= 1) {
                producer_loop_single_thread();
            } else {
                producer_loop_parallel();
            }
        }});
    }

    /// Borrow the next ordered chunk and report whether it carries an error.
    [[nodiscard]] std::pair<const decoded_chunk*, bool> get_next_chunk() noexcept {
        start_producer();
        std::unique_lock<std::mutex> guard(_state_mutex);
        // Tagged wait: the consumer reads chunks in chunk-index order
        // regardless of which slot they live in. Workers and the drain
        // mark slots ready as soon as their chunk's resolution finishes,
        // possibly out of order. The consumer waits for its specific tag.
        const std::size_t slot = _consumer_next_index % chunk_arena_capacity;
        _state_cv.wait(guard, [this, slot]() noexcept {
            return (_slot_lifecycles[slot] == detail::slot_lifecycle::ready_for_consumer &&
                    _slot_chunk_index[slot] == _consumer_next_index) ||
                   _producer_done.load(std::memory_order_acquire);
        });
        const bool slot_ready =
            _slot_lifecycles[slot] == detail::slot_lifecycle::ready_for_consumer &&
            _slot_chunk_index[slot] == _consumer_next_index;
        if (!slot_ready) {
            const bool has_error = !_last_error_message.empty();
            return {nullptr, has_error};
        }
        _slot_lifecycles[slot] = detail::slot_lifecycle::consumer_holding;
        ++_consumer_next_index;
        _state_cv.notify_all();
        decoded_chunk* chunk = _chunk_arena[slot].get();
        return {chunk, chunk->state == chunk_state::decode_error};
    }

    /// Release a borrowed chunk slot for producer reuse.
    void release_chunk(const decoded_chunk* chunk) noexcept {
        if (chunk == nullptr)
            return;
        std::size_t slot = chunk_arena_capacity;
        for (std::size_t i = 0; i < chunk_arena_capacity; ++i) {
            if (_chunk_arena[i].get() == chunk) {
                slot = i;
                break;
            }
        }
        if (slot >= chunk_arena_capacity)
            return;
        std::lock_guard<std::mutex> guard(_state_mutex);
        _slot_lifecycles[slot] = detail::slot_lifecycle::free;
        _slot_chunk_index[slot] = std::numeric_limits<std::size_t>::max();
        _state_cv.notify_all();
    }

    /// Return the first producer or decoder diagnostic.
    [[nodiscard]] std::string last_error_message() const noexcept {
        std::lock_guard<std::mutex> guard(_state_mutex);
        return _last_error_message;
    }

    /// Compatibility hook. Chunks are released explicitly by pointer.
    void release_until(std::size_t /*encoded_bit_offset*/) noexcept {}

    /// Stop the producer and join its task group. Idempotent.
    void shutdown() noexcept {
        if (!_producer_started)
            return;
        _shutdown.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> guard(_state_mutex);
            _state_cv.notify_all();
        }
        _producer_group.wait();
        _producer_started = false;
    }

  private:
    // -----------------------------------------------------------------
    // Producer (single-worker path)
    // -----------------------------------------------------------------

    /// Single-worker per-member loop. Each chunk moves directly from worker
    /// ownership to consumer readiness because no ordered marker drain is needed.
    void producer_loop_single_thread() noexcept {
        bit_reader reader{_compressed_view};
        std::size_t fill_cursor = 0;         // round-robin slot index
        std::size_t chunk_index_counter = 0; // Strict consumer order.

        while (true) {
            auto header_or = parse_gzip_header(reader, _compressed_view);
            if (!header_or) {
                set_error("gzip header parse failed: " + header_or.error());
                signal_done();
                return;
            }
            deflate_block_decoder decoder;
            decoder.reset();
            decoder.set_initial_window(std::span<const std::uint8_t>{});

            while (!_shutdown.load(std::memory_order_acquire)) {
                const std::size_t slot = fill_cursor;
                const std::size_t this_chunk_index = chunk_index_counter;

                {
                    std::unique_lock<std::mutex> guard(_state_mutex);
                    _state_cv.wait(guard, [this, slot]() noexcept {
                        return _shutdown.load(std::memory_order_acquire) ||
                               _slot_lifecycles[slot] == detail::slot_lifecycle::free;
                    });
                    if (_shutdown.load(std::memory_order_acquire))
                        return;
                    _slot_lifecycles[slot] = detail::slot_lifecycle::worker_filling;
                    _slot_chunk_index[slot] = this_chunk_index;
                }
                fill_cursor = (fill_cursor + 1) % chunk_arena_capacity;
                ++chunk_index_counter;

                decoded_chunk& chunk = *_chunk_arena[slot];
                chunk.encoded_start_bit_offset = reader.bit_position();
                chunk.state = chunk_state::directly_decoded;
                chunk.uses_marker_storage = false;
                chunk.byte_storage.clear();
                chunk.marker_storage.clear();
                chunk.is_member_final = false;
                chunk.decompressed_size_bytes = 0;
                chunk.error_message.assign(std::string_view{});

                if (auto reserve_or = chunk.byte_storage.reserve(_chunk_target_bytes);
                    !reserve_or) {
                    chunk.state = chunk_state::decode_error;
                    chunk.error_message.assign(reserve_or.error());
                    set_error(reserve_or.error());
                    publish_to_consumer(slot);
                    signal_done();
                    return;
                }

                const auto [produced, status] =
                    detail::decode_until(decoder, reader, chunk, _chunk_target_bytes);
                chunk.decompressed_size_bytes = produced;
                chunk.encoded_end_bit_offset = reader.bit_position();
                if (status != decode_status::ok && status != decode_status::end_of_block &&
                    status != decode_status::end_of_stream) {
                    chunk.state = chunk_state::decode_error;
                    chunk.error_message.assign(decode_status_name(status));
                    set_error(std::string{decode_status_name(status)});
                    publish_to_consumer(slot);
                    signal_done();
                    return;
                }

                if (!chunk.uses_marker_storage) {
                    const std::size_t window_bytes =
                        std::min<std::size_t>(chunk.byte_storage.size, chunk_trailing_window_bytes);
                    if (window_bytes > 0) {
                        const std::uint8_t* src = chunk.byte_storage.data.get() +
                                                  (chunk.byte_storage.size - window_bytes);
                        std::memcpy(chunk.trailing_window.data(), src, window_bytes);
                    }
                    chunk.trailing_window_size = window_bytes;
                }

                const bool stream_end = decoder.is_end_of_block() && decoder.is_last_block();
                if (stream_end) {
                    chunk.is_member_final = true;
                    publish_to_consumer(slot);
                    reader.align_to_byte_boundary();
                    if (reader.bits_buffered() < 64)
                        reader.refill();
                    if (reader.bits_buffered() >= 64) {
                        (void)reader.read_aligned_le_u64();
                    }
                    break;
                }
                publish_to_consumer(slot);
            }

            const std::size_t consumed_bytes = reader.bit_position() / 8u;
            if (consumed_bytes >= _compressed_view.size())
                break;
        }

        signal_done();
    }

    // -----------------------------------------------------------------
    // Producer (parallel-pool path)
    // -----------------------------------------------------------------

    /// Plan independent chunks, decode them concurrently, and drain in order.
    void producer_loop_parallel() noexcept {
        auto members_or = detail::prescan_members(_compressed_view);
        if (!members_or) {
            set_error(members_or.error());
            signal_done();
            return;
        }
        auto plans_or =
            detail::plan_chunks(_compressed_view, *members_or, default_chunk_target_bytes);
        if (!plans_or) {
            set_error(plans_or.error());
            signal_done();
            return;
        }
        std::vector<detail::chunk_decode_plan> plans = std::move(*plans_or);

        const std::size_t total_chunks = plans.size();
        if (total_chunks == 0) {
            signal_done();
            return;
        }

        _next_worker_chunk_index.store(0, std::memory_order_relaxed);

        auto& pool = bioseq::parallelism::global_pool::instance();
        if (!pool.is_active()) {
            pool.configure(bioseq::parallelism::pool_config{
                .maximum_concurrency =
                    static_cast<unsigned>(std::max<std::size_t>(1, _worker_count)),
                .mode = bioseq::parallelism::pipeline_concurrency_mode::dispatch_parallel,
            });
        }
        auto workers = pool.make_task_group();
        for (std::size_t w = 0; w < _worker_count; ++w) {
            workers.run(bioseq::parallelism::task_body{
                [this, &plans, total_chunks]() noexcept { worker_loop(plans, total_chunks); }});
        }

        // Drain in chunk-index order, resolve markers, publish to consumer.
        std::array<std::uint8_t, chunk_trailing_window_bytes> drain_window{};
        std::size_t drain_window_size = 0;
        std::uint32_t current_member_index = 0;
        bool fatal_error = false;

        for (std::size_t k = 0;
             k < total_chunks && !_shutdown.load(std::memory_order_acquire) && !fatal_error;
             ++k) {
            const std::size_t slot = k % chunk_arena_capacity;

            // Wait for slot[k % K] to be filled by the worker that owns
            // chunk_index == k.
            {
                std::unique_lock<std::mutex> guard(_state_mutex);
                _state_cv.wait(guard, [this, slot, k]() noexcept {
                    return _shutdown.load(std::memory_order_acquire) ||
                           (_slot_lifecycles[slot] == detail::slot_lifecycle::ready_for_drain &&
                            _slot_chunk_index[slot] == k);
                });
                if (_shutdown.load(std::memory_order_acquire))
                    break;
            }

            decoded_chunk& chunk = *_chunk_arena[slot];
            const auto& plan = plans[k];

            // Member-boundary reset.
            if (plan.member_index != current_member_index) {
                drain_window_size = 0;
                current_member_index = plan.member_index;
            }

            // Worker reported an error?
            if (chunk.state == chunk_state::decode_error) {
                set_error(std::string{chunk.error_message.view()});
                publish_to_consumer(slot);
                fatal_error = true;
                break;
            }

            // Resolve the trailing 32 KiB in encoded order so the window
            // propagates forward correctly. The remaining prefix is resolved
            // by the worker after this slot is marked ready.
            if (chunk.uses_marker_storage) {
                if (!detail::resolve_trailing_window_markers(
                        chunk, drain_window.data(), drain_window_size)) {
                    chunk.state = chunk_state::decode_error;
                    chunk.error_message.assign(std::string_view{"resolve_markers: alloc failure"});
                    set_error("resolve_markers: alloc failure");
                    publish_to_consumer(slot);
                    fatal_error = true;
                    break;
                }
            }

            // Update drain_window from the chunk's trailing window.
            const std::size_t chunk_tail = chunk.trailing_window_size;
            if (chunk_tail >= chunk_trailing_window_bytes) {
                std::memcpy(
                    drain_window.data(), chunk.trailing_window.data(), chunk_trailing_window_bytes);
                drain_window_size = chunk_trailing_window_bytes;
            } else if (chunk_tail > 0u) {
                const std::size_t keep = std::min<std::size_t>(
                    drain_window_size, chunk_trailing_window_bytes - chunk_tail);
                if (keep > 0u) {
                    std::array<std::uint8_t, chunk_trailing_window_bytes> scratch{};
                    std::memcpy(
                        scratch.data(), drain_window.data() + (drain_window_size - keep), keep);
                    std::memcpy(drain_window.data(), scratch.data(), keep);
                }
                std::memcpy(drain_window.data() + keep, chunk.trailing_window.data(), chunk_tail);
                drain_window_size =
                    std::min<std::size_t>(keep + chunk_tail, chunk_trailing_window_bytes);
            }

            if (chunk.is_member_final) {
                drain_window_size = 0;
            }

            chunk.state = chunk_state::directly_decoded;
            if (chunk.uses_marker_storage) {
                // Hand the remaining prefix back to its worker. The worker
                // publishes the slot after marker resolution.
                std::lock_guard<std::mutex> guard(_state_mutex);
                _slot_lifecycles[slot] = detail::slot_lifecycle::trailing_window_resolved;
                _state_cv.notify_all();
            } else {
                // Direct-mode chunk — already fully resolved by the
                // worker. Publish it directly.
                publish_to_consumer(slot);
            }
        }

        // Tell workers to stop.
        _shutdown.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> guard(_state_mutex);
            _state_cv.notify_all();
        }
        workers.wait();

        signal_done();
    }

    /// One worker task. Claims chunks atomically, decodes them into
    /// their tagged arena slots, and signals the ordered drain.
    void worker_loop(const std::vector<detail::chunk_decode_plan>& plans,
                     std::size_t total_chunks) noexcept {
        // Keep the decoder's 128 KiB window out of the worker stack.
        auto decoder = std::make_unique<deflate_block_decoder>();

        while (true) {
            const std::size_t chunk_index =
                _next_worker_chunk_index.fetch_add(1u, std::memory_order_relaxed);
            if (chunk_index >= total_chunks)
                break;
            if (_shutdown.load(std::memory_order_acquire))
                break;

            const std::size_t slot = chunk_index % chunk_arena_capacity;

            // Wait for slot to be FREE (consumer released previous
            // chunk).
            {
                std::unique_lock<std::mutex> guard(_state_mutex);
                _state_cv.wait(guard, [this, slot]() noexcept {
                    return _shutdown.load(std::memory_order_acquire) ||
                           _slot_lifecycles[slot] == detail::slot_lifecycle::free;
                });
                if (_shutdown.load(std::memory_order_acquire))
                    break;
                _slot_lifecycles[slot] = detail::slot_lifecycle::worker_filling;
                _slot_chunk_index[slot] = chunk_index;
            }

            decoded_chunk& chunk = *_chunk_arena[slot];
            const auto err =
                detail::worker_decode_chunk(*decoder, _compressed_view, plans[chunk_index], chunk);
            if (err != decode_status::ok) {
                chunk.state = chunk_state::decode_error;
                chunk.error_message.assign(decode_status_name(err));
            } else {
                chunk.state = chunk_state::directly_decoded;
            }

            const bool needs_remaining_marker_resolution =
                chunk.uses_marker_storage && err == decode_status::ok;

            {
                std::lock_guard<std::mutex> guard(_state_mutex);
                _slot_lifecycles[slot] = detail::slot_lifecycle::ready_for_drain;
                _state_cv.notify_all();
            }

            // If markers remain, wait until the drain has propagated and
            // snapshotted the dependency window. A ready slot wins over a
            // simultaneous shutdown notification: otherwise the final chunk's
            // prefix would remain unresolved and be silently omitted.
            if (needs_remaining_marker_resolution) {
                bool trailing_window_ready = false;
                {
                    std::unique_lock<std::mutex> guard(_state_mutex);
                    _state_cv.wait(guard, [this, slot, chunk_index]() noexcept {
                        return _shutdown.load(std::memory_order_acquire) ||
                               (_slot_lifecycles[slot] ==
                                    detail::slot_lifecycle::trailing_window_resolved &&
                                _slot_chunk_index[slot] == chunk_index);
                    });
                    trailing_window_ready = _slot_lifecycles[slot] ==
                                                detail::slot_lifecycle::trailing_window_resolved &&
                                            _slot_chunk_index[slot] == chunk_index;
                }
                if (!trailing_window_ready)
                    break;
                detail::resolve_remaining_markers(chunk);
                publish_to_consumer(slot);
            }
        }
    }

    // -----------------------------------------------------------------
    // Shared producer helpers
    // -----------------------------------------------------------------

    /// Publish a slot to the consumer. Slot's chunk is fully resolved.
    /// The consumer reads tagged slots in chunk-index order even when workers
    /// publish them out of order.
    void publish_to_consumer(std::size_t slot) noexcept {
        std::lock_guard<std::mutex> guard(_state_mutex);
        _slot_lifecycles[slot] = detail::slot_lifecycle::ready_for_consumer;
        _state_cv.notify_all();
    }

    /// Mark producer completion and wake every slot waiter.
    void signal_done() noexcept {
        _producer_done.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> guard(_state_mutex);
        _state_cv.notify_all();
    }

    /// Retain the first terminal error and discard later diagnostics.
    void set_error(std::string message) noexcept {
        std::lock_guard<std::mutex> guard(_state_mutex);
        if (_last_error_message.empty()) {
            _last_error_message = std::move(message);
        }
    }

    // -----------------------------------------------------------------
    // Members
    std::span<const std::uint8_t> _compressed_view;
    std::size_t _chunk_target_bytes;
    std::size_t _worker_count;
    bool _verify_crc;

    std::vector<std::unique_ptr<decoded_chunk>> _chunk_arena;

    mutable std::mutex _state_mutex;
    std::condition_variable _state_cv;

    /// Per-slot lifecycle. Source of truth for slot ownership.
    std::vector<detail::slot_lifecycle> _slot_lifecycles;
    /// chunk_index currently in slot[i] (max_size_t = unset). Set by
    /// workers, cleared on consumer release.
    std::vector<std::size_t> _slot_chunk_index;

    // Consumer's next-to-read chunk_index. Strictly monotonic.
    std::size_t _consumer_next_index = 0;

    // Parallel-path coordination.
    std::atomic<std::size_t> _next_worker_chunk_index{0};

    std::atomic<bool> _shutdown{false};
    std::atomic<bool> _producer_done{false};
    bioseq::parallelism::scoped_task_group _producer_group;
    bool _producer_started = false;
    std::string _last_error_message;
};

} // namespace bioseq::io::compression
