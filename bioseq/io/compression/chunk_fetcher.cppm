/**
 * @file chunk_fetcher.cppm
 * @brief Decode gzip chunks and return them in input order.
 *
 * Parallel workers can start with references to earlier output still unresolved.
 * An ordered pass resolves each chunk's last 32 KiB so the next chunk can use
 * it as history. Workers then resolve the remaining bytes.
 *
 * Chunk tags distinguish successive uses of the same buffer slot.
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

/// Maximum number of chunks held by workers and the consumer.
inline constexpr std::size_t chunk_arena_capacity = 8;

/// Default chunk target: output bytes for one worker, compressed bytes for planning.
inline constexpr std::size_t default_chunk_target_bytes = 32 * 1024 * 1024;

/// Upper bound on concurrent decoders sharing the fixed-capacity arena.
inline constexpr std::size_t maximum_worker_count = 12;

/// Output-size threshold checked between parallel decode steps.
inline constexpr std::size_t maximum_chunk_decompressed_bytes = 256 * 1024 * 1024;

namespace detail {

// Chunk slot ownership states.

/// Which producer or consumer stage currently owns a chunk slot.
enum class slot_lifecycle : std::uint8_t {
    free = 0,
    worker_filling = 1,
    ready_for_drain = 2,
    trailing_window_resolved = 3,
    ready_for_consumer = 4,
    consumer_holding = 5,
};

// Find possible gzip member headers.

/// Header location and first DEFLATE bit for a possible gzip member.
struct gzip_member_descriptor {
    /// Byte offset of the gzip-magic byte (0x1f).
    std::size_t header_byte = 0;
    /// First bit after the gzip header, where DEFLATE decoding begins.
    std::size_t header_end_bit = 0;
};

/// Check a dynamic-Huffman header and its code lengths. Advance the reader
/// while validating the tables, without building literal or distance lookups.
[[nodiscard]] inline bool validate_dynamic_huffman_only(bit_reader& reader) noexcept;

/// Find possible gzip members by checking magic bytes, headers, and the first
/// block's Huffman code lengths. Only members starting with a dynamic-Huffman
/// block are accepted here. The single-worker path supports other block types.
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

        // Check the gzip magic, compression method, and reserved flag bits.
        if (compressed_view[cursor + 0] != 0x1F || compressed_view[cursor + 1] != 0x8B ||
            compressed_view[cursor + 2] != 0x08 || compressed_view[cursor + 3] > 0x1F) {
            ++cursor;
            continue;
        }

        // Parse optional header fields before locating the first DEFLATE block.
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

        // Validate the first block's code lengths to reject accidental gzip magic
        // inside compressed data. This remains a candidate check until decoding.
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

        // Continue searching after this header for the next member.
        cursor = info.header_end_bit / 8u;
        if ((info.header_end_bit % 8u) != 0u)
            ++cursor;
    }

    if (members.empty()) {
        return std::unexpected(std::string{"prescan_members: no gzip member detected in input"});
    }
    return members;
}

// Choose chunk boundaries.

/// Compressed bit range and member position assigned to a worker.
struct chunk_decode_plan {
    std::uint32_t member_index = 0;
    /// Candidate block start that passed the header checks.
    std::size_t start_bit = 0;
    /// Target stopping boundary. A worker can continue through fixed-Huffman
    /// blocks and the member's final block.
    std::size_t stop_at_bit = 0;
    /// True for the first chunk, which starts with empty decoding history.
    bool is_first_in_member = false;
    /// True for the last planned chunk of this member.
    bool is_last_in_member = false;
};

/// Check a dynamic-Huffman header and code lengths, advancing the reader.
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
        // Select the precode ordering array in the parent namespace. The detail
        // namespace also has a precode_alphabet name for an alphabet size.
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

/// Split each member near the requested compressed-byte intervals. The first
/// chunk starts after its header. Later chunks start at candidates whose
/// literal and distance code lengths pass validation. Each planned range ends
/// at the next start, the next member header, or the end of input.
[[nodiscard]] inline std::expected<std::vector<chunk_decode_plan>, std::string>
plan_chunks(std::span<const std::uint8_t> compressed_view,
            const std::vector<gzip_member_descriptor>& members,
            std::size_t chunk_target_compressed_bytes) noexcept {
    std::vector<chunk_decode_plan> plans;
    if (members.empty()) {
        return std::unexpected(std::string{"plan_chunks: no members"});
    }

    const std::size_t total_bits = compressed_view.size() * 8u;

    // Skip rejected candidates until a header passes the code-length checks.
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

    // Choose a planning concurrency limit when setting up an inactive pool.
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

// Resolve references to earlier output.

/// Convert literal bytes and references to earlier output into bytes.
/// References beyond prev_window_size read as zero from the padded history.
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

/// Resolve the chunk's last 32 KiB so the next chunk can use that history.
/// Save the preceding history for a worker to resolve the remaining prefix.
/// Call this in chunk order with empty byte_storage.
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

/// Resolve the remaining references using the saved history.
/// Different chunks can finish this step independently.
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

// Copy decoder output into chunk storage.

/// Append resolved bytes. Return the byte count, or max_size_t on allocation failure.
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

/// Append marker values. Return their count, or max_size_t on allocation failure.
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

/// Decode toward the requested output size for the single-worker producer.
/// Stop at the member's final block or a decoder status that needs handling.
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

// Decode one planned chunk on a pool worker.

/// Decode a planned range using the worker's reusable decoder. The first
/// chunk has empty history. Later chunks keep references to earlier output
/// until the ordered pass can supply those bytes.
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
        // A member starts with empty history, so its first chunk can produce bytes directly.
        decoder.reset();
        decoder.set_initial_window(std::span<const std::uint8_t>{});
    } else {
        // Start with unknown history represented by markers. If a candidate header
        // fails to decode, try another candidate in this range. Reset once so retries
        // can reuse the marker buffer while read_header replaces the header state.
        decoder.reset();
        decode_status header_err = decode_status::ok;
        std::size_t retry_search_start = plan.start_bit;
        // Retry until a header is accepted or this range has no candidates left.
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
                // Return an empty chunk when no candidate remains in this planned range.
                out_chunk.encoded_end_bit_offset = plan.stop_at_bit;
                return decode_status::ok;
            }
            retry_search_start = next_candidate;
        }
        out_chunk.encoded_start_bit_offset = retry_search_start;
    }

    while (true) {
        if (decoder.is_end_of_block()) {
            // Save the block start before read_header advances into its header.
            const auto next_block_bit = reader.bit_position();
            const auto err = decoder.read_header(reader);
            if (err != decode_status::ok) {
                return err;
            }
            // Keep decoding intervening fixed-Huffman blocks because the planner only
            // chooses dynamic-Huffman starts. The final block is also decoded before stopping.
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
                // The decoder now returns bytes, but earlier output still contains unresolved
                // references. Append the bytes as 16-bit literal values so the ordered pass
                // can resolve the whole chunk without losing those earlier references.
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
                // This output has no unresolved references and can use byte storage.
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
            // BFINAL ends this member. The parallel path stops before the gzip footer.
            out_chunk.is_member_final = true;
            break;
        }
        // Stop oversized chunks between decode steps. A false block candidate can
        // otherwise keep producing output far beyond the intended chunk size.
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

// Ordered chunk delivery.

/// Decode gzip chunks and lend them to the consumer in input order.
/// The compressed bytes must remain valid until the fetcher is destroyed.
class chunk_fetcher {
  public:
    /// Borrow compressed input and allocate reusable chunk slots. The output-size
    /// target applies to the single-worker path. Parallel planning uses its default.
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

    /// Copying would share ownership of active tasks and chunk slots.
    chunk_fetcher(const chunk_fetcher&) = delete;

    /// Copy assignment is disabled because task and chunk ownership is exclusive.
    chunk_fetcher& operator=(const chunk_fetcher&) = delete;

    /// Moving is disabled because background tasks keep this object's address.
    chunk_fetcher(chunk_fetcher&&) = delete;

    /// Move assignment is disabled while tasks can refer to this object.
    chunk_fetcher& operator=(chunk_fetcher&&) = delete;

    /// Start the producer if it has not already been started.
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

    /// Wait for the next chunk and borrow it until release_chunk is called.
    /// A null pointer means production ended. The Boolean reports an error in
    /// the returned chunk or, when null, in the producer.
    [[nodiscard]] std::pair<const decoded_chunk*, bool> get_next_chunk() noexcept {
        start_producer();
        std::unique_lock<std::mutex> guard(_state_mutex);
        // Wait for the expected chunk tag. A slot may hold a later chunk after reuse,
        // and workers can finish in a different order from the compressed input.
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

    /// Do nothing. The consumer releases chunks by pointer through release_chunk.
    void release_until(std::size_t /*encoded_bit_offset*/) noexcept {}

    /// Request shutdown and wait for the producer task group to finish.
    /// Repeated calls after shutdown do nothing.
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
    // Single-worker producer.

    /// Decode members in order with one decoder. Publish each completed chunk
    /// directly because the decoder already knows the preceding output.
    void producer_loop_single_thread() noexcept {
        bit_reader reader{_compressed_view};
        std::size_t fill_cursor = 0;         // round-robin slot index
        std::size_t chunk_index_counter = 0; // Index used to preserve consumer order.

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

    // Parallel producer.

    /// Plan chunks, decode them on workers, and pass history forward in input order.
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

        // Resolve each chunk's history in input order before allowing delivery.
        std::array<std::uint8_t, chunk_trailing_window_bytes> drain_window{};
        std::size_t drain_window_size = 0;
        std::uint32_t current_member_index = 0;
        bool fatal_error = false;

        for (std::size_t k = 0;
             k < total_chunks && !_shutdown.load(std::memory_order_acquire) && !fatal_error;
             ++k) {
            const std::size_t slot = k % chunk_arena_capacity;

            // Wait for the worker to fill the slot with the expected chunk tag.
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

            // Each gzip member starts with empty history.
            if (plan.member_index != current_member_index) {
                drain_window_size = 0;
                current_member_index = plan.member_index;
            }

            // Publish a worker error and stop decoding.
            if (chunk.state == chunk_state::decode_error) {
                set_error(std::string{chunk.error_message.view()});
                publish_to_consumer(slot);
                fatal_error = true;
                break;
            }

            // Resolve the tail first so the next chunk can use it as history. The worker
            // can finish this chunk's prefix after receiving the saved preceding history.
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

            // Keep the last 32 KiB across chunks, including earlier bytes when this chunk is short.
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
                // All bytes are resolved, so the consumer can read this chunk.
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

    /// Claim chunk indices, decode into reusable slots, and notify the ordered pass.
    /// Finish any remaining marker resolution after that pass supplies the history.
    void worker_loop(const std::vector<detail::chunk_decode_plan>& plans,
                     std::size_t total_chunks) noexcept {
        // Allocate the decoder on the heap to keep its large history buffer off the stack.
        auto decoder = std::make_unique<deflate_block_decoder>();

        while (true) {
            const std::size_t chunk_index =
                _next_worker_chunk_index.fetch_add(1u, std::memory_order_relaxed);
            if (chunk_index >= total_chunks)
                break;
            if (_shutdown.load(std::memory_order_acquire))
                break;

            const std::size_t slot = chunk_index % chunk_arena_capacity;

            // Wait until the consumer releases the slot's previous chunk.
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

            // Wait for the preceding history before resolving the prefix. If the history
            // is ready when shutdown arrives, finish and publish the chunk so its last
            // bytes are still delivered.
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

    // Shared producer helpers.

    /// Mark a resolved chunk or an error ready for the consumer. Chunk tags let
    /// the consumer keep input order even when publication happens out of order.
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

    /// Keep the first terminal error and ignore later diagnostics.
    void set_error(std::string message) noexcept {
        std::lock_guard<std::mutex> guard(_state_mutex);
        if (_last_error_message.empty()) {
            _last_error_message = std::move(message);
        }
    }

    // Fetcher state.
    std::span<const std::uint8_t> _compressed_view;
    std::size_t _chunk_target_bytes;
    std::size_t _worker_count;
    bool _verify_crc;

    std::vector<std::unique_ptr<decoded_chunk>> _chunk_arena;

    mutable std::mutex _state_mutex;
    std::condition_variable _state_cv;

    /// Current owner or processing stage of each chunk slot.
    std::vector<detail::slot_lifecycle> _slot_lifecycles;
    /// Chunk index in each slot, or max_size_t when unused. Producers set the
    /// tag, and consumer release clears it.
    std::vector<std::size_t> _slot_chunk_index;

    // Index of the next chunk the consumer expects.
    std::size_t _consumer_next_index = 0;

    // Next chunk index to claim in the parallel producer.
    std::atomic<std::size_t> _next_worker_chunk_index{0};

    std::atomic<bool> _shutdown{false};
    std::atomic<bool> _producer_done{false};
    bioseq::parallelism::scoped_task_group _producer_group;
    bool _producer_started = false;
    std::string _last_error_message;
};

} // namespace bioseq::io::compression
