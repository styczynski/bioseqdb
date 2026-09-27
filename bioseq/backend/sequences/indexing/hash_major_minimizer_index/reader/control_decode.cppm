/**
 * @file hash_major_minimizer_index/reader/control_decode.cppm
 * @brief Check control blocks and select the hashes requested by the, query.
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

export module bioseq.indexing.hash_major_minimizer_index:reader_control_decode;

import :reader_contract;
import :codec;
import :planner;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.storage;
import bioseq.parallelism.memory_admission;

export namespace bioseq::indexing {

/// Check the next control block and record counts for matching query hashes.
///
/// Check the block contents against their checksum and directory entry. Counts
/// at most max_occ receive posting offsets for later reads. Larger counts stay
/// available to  lookup, with their positions marked unavailable. A failure
/// stops further delivery.
[[nodiscard]] inline bool hash_major_minimizer_selective_reader::consume_control_block_(
    void* opaque, std::uint32_t range_ordinal, std::span<const std::byte> encoded_control) {
    using namespace hash_major_minimizer_detail;
    auto& context = *static_cast<control_delivery_context*>(opaque);
    if (context.error)
        return false;
    if (range_ordinal != context.next_range || range_ordinal >= context.ranges->size()) {
        context.error = hash_major_minimizer_error::source_read;
        return false;
    }
    const auto& range = (*context.ranges)[range_ordinal];
    if (range.reserved_zero != 0u || range.control_block_ordinal >= context.directory->size()) {
        context.error = hash_major_minimizer_error::control;
        return false;
    }
    const auto& entry = (*context.directory)[static_cast<std::size_t>(range.control_block_ordinal)];
    if (entry.control_block_ordinal != range.control_block_ordinal ||
        entry.control_byte_offset != range.control_byte_offset ||
        entry.control_byte_length != range.byte_count ||
        encoded_control.size() != range.byte_count) {
        context.error = hash_major_minimizer_error::source_read;
        return false;
    }
    ++context.controls_read;
    context.control_stream_crc->update(encoded_control);
    auto control = decode_hash_major_minimizer_control_block_into(
        encoded_control,
        context.manifest->storage_profile.hash_width_bytes,
        context.manifest->storage_profile.stored_posting_width_bytes,
        *context.scratch);
    if (!control) {
        context.error = control.error();
        return false;
    }
    const std::uint64_t maximum_hash =
        hash_major_minimizer_max_hash(context.manifest->storage_profile);
    std::uint64_t expected_posting_bytes = 0u;
    if (control->header.first_hash != entry.first_hash ||
        control->hashes.back() != entry.last_hash || control->hashes.back() > maximum_hash ||
        control->header.control_count != entry.control_count ||
        control->header.exact_posting_count != entry.exact_posting_count ||
        control->header.posting_byte_offset != entry.posting_byte_offset ||
        !checked_multiply(entry.exact_posting_count,
                          context.manifest->storage_profile.stored_posting_width_bytes,
                          expected_posting_bytes) ||
        expected_posting_bytes != entry.posting_byte_length ||
        hash_major_minimizer_block_crc64(entry,
                                         control->header.control_crc64,
                                         control->header.posting_crc64) != entry.block_crc64) {
        context.error = hash_major_minimizer_error::directory;
        return false;
    }
    if (context.authorities->size() == context.authorities->capacity()) {
        context.error = hash_major_minimizer_error::admission;
        return false;
    }
    context.authorities->push_back(block_authority{
        .directory_ordinal = range.control_block_ordinal,
        .posting_byte_offset = entry.posting_byte_offset,
        .posting_byte_length = entry.posting_byte_length,
        .posting_crc64 = control->header.posting_crc64,
    });
    context.query_cursor =
        context.reader->canonical_lower_bound_(context.query_cursor, entry.first_hash);
    std::uint64_t posting_prefix = 0u;
    for (std::size_t control_ordinal = 0u; control_ordinal < control->hashes.size();
         ++control_ordinal) {
        const std::uint64_t hash = control->hashes[control_ordinal];
        const std::uint64_t count = control->exact_counts[control_ordinal];
        while (context.query_cursor < context.query_hashes.size() &&
               context.query_hashes.at(context.query_cursor) < hash) {
            ++context.query_cursor;
        }
        if (context.query_cursor < context.query_hashes.size() &&
            context.query_hashes.at(context.query_cursor) == hash) {
            if (count == 0u) {
                context.error = hash_major_minimizer_error::count;
                return false;
            }
            const std::size_t query_ordinal = context.query_cursor;
            if (context.reader->primary_counts_[query_ordinal] != 0u ||
                context.reader->query_offsets_[query_ordinal] != k_absent_query_offset) {
                context.error = hash_major_minimizer_error::order;
                return false;
            }
            ++context.reader->selected_hash_count_;
            if (count <= 65'534u) {
                context.reader->primary_counts_[query_ordinal] = static_cast<std::uint16_t>(count);
            } else {
                context.reader->primary_counts_[query_ordinal] = 65'535u;
                if (context.reader->count_exceptions_.size() ==
                    context.reader->count_exceptions_.capacity()) {
                    context.error = hash_major_minimizer_error::admission;
                    return false;
                }
                context.reader->count_exceptions_.push_back(exact_global_count_exception{
                    .query_ordinal = static_cast<std::uint32_t>(query_ordinal),
                    .reserved_zero = 0u,
                    .exact_count = count,
                });
            }
            if (count <= context.max_occ) {
                std::uint64_t posting_delta = 0u;
                std::uint64_t posting_offset = 0u;
                if (!checked_multiply(posting_prefix,
                                      context.manifest->storage_profile.stored_posting_width_bytes,
                                      posting_delta) ||
                    !checked_add(entry.posting_byte_offset, posting_delta, posting_offset) ||
                    posting_offset >= k_unavailable_query_offset) {
                    context.error = hash_major_minimizer_error::arithmetic_overflow;
                    return false;
                }
                context.reader->query_offsets_[query_ordinal] = posting_offset;
                ++context.reader->memory_stats_.accepted_hash_count;
            } else {
                context.reader->query_offsets_[query_ordinal] = k_unavailable_query_offset;
            }
            ++context.query_cursor;
        }
        if (!checked_add(posting_prefix, count, posting_prefix)) {
            context.error = hash_major_minimizer_error::arithmetic_overflow;
            return false;
        }
    }
    if (posting_prefix != entry.exact_posting_count) {
        context.error = hash_major_minimizer_error::count;
        return false;
    }
    ++context.next_range;
    return true;
}

} // namespace bioseq::indexing
