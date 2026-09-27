/**
 * @file hash_major_minimizer_index/reader/posting_decode.cppm
 * @brief Check delivered posting bytes and fill the saved position arrays.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.indexing.hash_major_minimizer_index:reader_posting_decode;

import :reader_contract;
import :codec;
import :planner;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.storage;
import bioseq.parallelism.memory_admission;

export namespace bioseq::indexing {

/// Decode the next fragment of a selected posting range into the reader's
/// position arrays.
///
/// Fragments must arrive in order and cover the requested bytes exactly. Each
/// hash's encoded positions must increase and stay within the target. A range
/// covering an entire block is also checked against its posting checksum.
[[nodiscard]] inline bool
hash_major_minimizer_selective_reader::consume_posting_bytes_(void* opaque,
                                                              std::uint32_t range_ordinal,
                                                              std::uint64_t range_byte_offset,
                                                              std::span<const std::byte> bytes) {
    using namespace hash_major_minimizer_detail;
    auto& context = *static_cast<posting_delivery_context*>(opaque);
    if (context.error)
        return false;
    if (range_ordinal != context.next_range || range_ordinal >= context.ranges->size() ||
        range_byte_offset != context.next_range_byte || bytes.empty() ||
        bytes.size() > k_hash_major_minimizer_read_slab_bytes) {
        context.error = hash_major_minimizer_error::source_read;
        return false;
    }
    const hash_major_minimizer_authorized_posting_range& range = (*context.ranges)[range_ordinal];
    const std::uint8_t width = context.manifest->storage_profile.stored_posting_width_bytes;
    if (range.reserved_zero != 0u || (range.flags & ~1u) != 0u ||
        range_byte_offset > range.byte_count ||
        bytes.size() > range.byte_count - range_byte_offset || bytes.size() % width != 0u) {
        context.error = hash_major_minimizer_error::posting;
        return false;
    }
    std::uint64_t absolute_chunk = 0u;
    if (!checked_add(range.posting_byte_offset, range_byte_offset, absolute_chunk)) {
        context.error = hash_major_minimizer_error::arithmetic_overflow;
        return false;
    }
    if (*context.posting_stream_complete) {
        if (absolute_chunk != *context.posting_stream_next) {
            *context.posting_stream_complete = false;
        } else {
            context.posting_stream_crc->update(bytes);
            *context.posting_stream_next += bytes.size();
        }
    }
    if ((range.flags & 1u) != 0u)
        context.range_crc.update(bytes);
    const std::size_t range_request_end =
        static_cast<std::size_t>(range.request_begin) + range.request_count;
    if (range_request_end > context.requests->size()) {
        context.error = hash_major_minimizer_error::posting;
        return false;
    }
    std::uint64_t absolute_chunk_end = 0u;
    if (!checked_add(absolute_chunk, bytes.size(), absolute_chunk_end)) {
        context.error = hash_major_minimizer_error::arithmetic_overflow;
        return false;
    }
    std::size_t chunk_byte_cursor = 0u;
    while (chunk_byte_cursor < bytes.size()) {
        if (context.next_request >= range_request_end) {
            context.error = hash_major_minimizer_error::posting;
            return false;
        }
        const posting_request& request = (*context.requests)[context.next_request];
        std::uint64_t request_bytes = 0u;
        std::uint64_t request_end = 0u;
        if (!checked_multiply(request.exact_count, width, request_bytes) ||
            !checked_add(request.posting_byte_offset, request_bytes, request_end)) {
            context.error = hash_major_minimizer_error::arithmetic_overflow;
            return false;
        }
        const std::uint64_t absolute = absolute_chunk + chunk_byte_cursor;
        if (request.exact_count == 0u || absolute < request.posting_byte_offset ||
            absolute >= request_end || (absolute - request.posting_byte_offset) % width != 0u ||
            request.query_ordinal >= context.reader->query_offsets_.size()) {
            context.error = hash_major_minimizer_error::posting;
            return false;
        }
        const std::uint64_t segment_end = std::min(absolute_chunk_end, request_end);
        const std::uint64_t segment_bytes = segment_end - absolute;
        const std::uint64_t posting_ordinal = (absolute - request.posting_byte_offset) / width;
        const std::uint64_t segment_position_count = segment_bytes / width;
        if (segment_bytes == 0u || segment_bytes % width != 0u ||
            posting_ordinal >= request.exact_count ||
            segment_position_count > request.exact_count - posting_ordinal) {
            context.error = hash_major_minimizer_error::posting;
            return false;
        }
        const std::uint64_t output_offset = context.reader->query_offsets_[request.query_ordinal];
        std::uint64_t destination = 0u;
        std::uint64_t destination_end = 0u;
        if (output_offset >= k_unavailable_query_offset ||
            !checked_add(output_offset, posting_ordinal, destination) ||
            !checked_add(destination, segment_position_count, destination_end)) {
            context.error = hash_major_minimizer_error::arithmetic_overflow;
            return false;
        }
        if (destination_end > context.reader->retained_position_count_) {
            context.error = hash_major_minimizer_error::posting;
            return false;
        }
        // Decode directly into the final array, preserving the stored integer width.
        const auto decode_segment = [&context,
                                     &bytes,
                                     chunk_byte_cursor,
                                     destination,
                                     destination_end,
                                     posting_ordinal]<class p_stored_position>() noexcept -> bool {
            p_stored_position* const retained_positions =
                [](hash_major_minimizer_selective_reader& reader) noexcept -> p_stored_position* {
                if constexpr (std::is_same_v<p_stored_position, std::uint32_t>) {
                    return reader.retained_positions32_.get();
                } else {
                    return reader.retained_positions64_.get();
                }
            }(*context.reader);
            bool have_previous = posting_ordinal != 0u;
            std::uint64_t previous_position =
                have_previous ? static_cast<std::uint64_t>(
                                    retained_positions[static_cast<std::size_t>(destination - 1u)])
                              : 0u;
            std::size_t input_offset = chunk_byte_cursor;
            std::size_t output_index = static_cast<std::size_t>(destination);
            const std::size_t output_end = static_cast<std::size_t>(destination_end);
            for (; output_index < output_end;
                 ++output_index, input_offset += sizeof(p_stored_position)) {
                const std::uint64_t position = read_le<p_stored_position>(bytes, input_offset);
                if ((position >> 1u) >= context.manifest->target_length ||
                    (have_previous && position <= previous_position)) {
                    context.error = (position >> 1u) >= context.manifest->target_length
                                        ? hash_major_minimizer_error::target_range
                                        : hash_major_minimizer_error::order;
                    return false;
                }
                retained_positions[output_index] = static_cast<p_stored_position>(position);
                previous_position = position;
                have_previous = true;
            }
            return true;
        };
        if (width == sizeof(std::uint32_t)) {
            if (!decode_segment.template operator()<std::uint32_t>()) {
                return false;
            }
        } else if (!decode_segment.template operator()<std::uint64_t>()) {
            return false;
        }
        chunk_byte_cursor += static_cast<std::size_t>(segment_bytes);
        if (segment_end == request_end) {
            ++context.next_request;
        }
    }
    context.next_range_byte += bytes.size();
    if (context.next_range_byte == range.byte_count) {
        if ((range.flags & 1u) != 0u &&
            context.range_crc.checksum() != range.expected_posting_crc64) {
            context.error = hash_major_minimizer_error::checksum;
            return false;
        }
        std::uint64_t ranges_delivered = 0u;
        std::uint64_t semantic_receipts = 0u;
        std::uint64_t crc_receipts = context.reader->memory_stats_.posting_range_crc_receipts;
        if (!checked_add(
                context.reader->memory_stats_.posting_ranges_delivered, 1u, ranges_delivered) ||
            !checked_add(
                context.reader->memory_stats_.posting_semantic_receipts, 1u, semantic_receipts) ||
            ((range.flags & 1u) != 0u && !checked_add(crc_receipts, 1u, crc_receipts))) {
            context.error = hash_major_minimizer_error::arithmetic_overflow;
            return false;
        }
        context.reader->memory_stats_.posting_ranges_delivered = ranges_delivered;
        context.reader->memory_stats_.posting_semantic_receipts = semantic_receipts;
        context.reader->memory_stats_.posting_range_crc_receipts = crc_receipts;
        ++context.next_range;
        context.next_range_byte = 0u;
        context.next_request = context.next_range < context.ranges->size()
                                   ? (*context.ranges)[context.next_range].request_begin
                                   : context.requests->size();
        context.range_crc = {};
    }
    return true;
}

} // namespace bioseq::indexing
