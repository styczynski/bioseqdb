/**
 * @file hash_major_minimizer_index/reader/selective_read.cppm
 * @brief Expected-generation validation, admitted selective reads, and reader assembly.
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

export module bioseq.indexing.hash_major_minimizer_index:reader_selective_read;

import :reader_contract;
import :reader_control_decode;
import :reader_posting_decode;
import :codec;
import :planner;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.storage;
import bioseq.parallelism.memory_admission;

export namespace bioseq::indexing {

/// Assemble a selective reader after authenticating generation identity and selected streams.
///
/// Directory bytes are checked completely. Only control blocks intersecting the canonical query
/// image and postings below `max_occ` are retained, with every allocation charged to `admission`.
[[nodiscard]] inline std::expected<hash_major_minimizer_selective_reader,
                                   hash_major_minimizer_error>
hash_major_minimizer_selective_reader::open(
    bioseq::parallelism::memory_admission_controller& admission,
    std::span<const std::byte> encoded_head,
    std::span<const std::byte> encoded_manifest,
    const expected_hash_major_minimizer_generation& expected,
    hash_major_minimizer_logical_stream_source_ref source,
    canonical_query_hash_view query_hashes,
    std::uint64_t max_occ) {
    using namespace hash_major_minimizer_detail;
    if (source.read_bytes == nullptr || source.logical_size == nullptr) {
        return std::unexpected(hash_major_minimizer_error::state);
    }
    auto head = decode_hash_major_minimizer_head(encoded_head);
    if (!head)
        return std::unexpected(head.error());
    auto manifest = decode_hash_major_minimizer_manifest(encoded_manifest);
    if (!manifest)
        return std::unexpected(manifest.error());
    const auto expected_digest =
        hash_major_minimizer_storage_profile_digest(expected.storage_profile);
    const auto expected_source = hash_major_minimizer_source_identity(expected.query_key,
                                                                      expected.source_epoch,
                                                                      expected.source_content_crc64,
                                                                      expected.target_length);
    if (!is_valid_hash_major_minimizer_storage_profile(expected.storage_profile,
                                                       expected.target_length) ||
        expected.generation_id == 0u || head->generation_id != expected.generation_id ||
        head->query_key != expected.query_key || head->source_epoch != expected.source_epoch ||
        head->source_content_crc64 != expected.source_content_crc64 ||
        head->source_identity != expected_source ||
        head->storage_profile_digest != expected_digest ||
        manifest->generation_id != expected.generation_id ||
        manifest->source_epoch != expected.source_epoch ||
        manifest->source_content_crc64 != expected.source_content_crc64 ||
        manifest->source_identity != expected_source ||
        manifest->target_length != expected.target_length ||
        manifest->storage_profile != expected.storage_profile ||
        manifest->storage_profile_digest != expected_digest) {
        return std::unexpected(hash_major_minimizer_error::expected_generation);
    }
    if (head->generation_id != manifest->generation_id ||
        head->source_epoch != manifest->source_epoch ||
        head->source_identity != manifest->source_identity ||
        head->source_content_crc64 != manifest->source_content_crc64 ||
        head->storage_profile_digest != manifest->storage_profile_digest ||
        head->manifest_payload_crc64 != crc64_ecma182(encoded_manifest) ||
        head->manifest_address_bound_row_crc64 !=
            crc64_index_chunk_row(head->generation_id,
                                  0,
                                  0,
                                  0u,
                                  static_cast<std::uint32_t>(encoded_manifest.size()),
                                  encoded_manifest)) {
        return std::unexpected(hash_major_minimizer_error::checksum);
    }
    if (!query_hashes.storage_matches_width() ||
        query_hashes.width_bytes != manifest->storage_profile.hash_width_bytes ||
        query_hashes.storage_profile_digest != expected_digest ||
        query_hashes.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(hash_major_minimizer_error::storage_contract_mismatch);
    }
    std::uint64_t previous_query = 0u;
    bool have_previous_query = false;
    const std::uint64_t maximum_hash = hash_major_minimizer_max_hash(manifest->storage_profile);
    for (std::size_t ordinal = 0u; ordinal < query_hashes.size(); ++ordinal) {
        const std::uint64_t hash = query_hashes.at(ordinal);
        if (hash > maximum_hash || (have_previous_query && hash <= previous_query)) {
            return std::unexpected(have_previous_query && hash <= previous_query
                                       ? hash_major_minimizer_error::order
                                       : hash_major_minimizer_error::storage_contract_mismatch);
        }
        previous_query = hash;
        have_previous_query = true;
    }
    hash_major_minimizer_selective_reader reader;
    reader.manifest_ = *manifest;
    reader.query_hashes_ = query_hashes;
    reader.memory_stats_.query_count = query_hashes.size();
    reader.memory_stats_.directory_entry_count = manifest->directory_entry_count;
    const std::uint64_t directory_bytes_u64 = manifest->streams[0].logical_bytes;
    if (directory_bytes_u64 > (std::numeric_limits<std::size_t>::max)() ||
        manifest->directory_entry_count > (std::numeric_limits<std::size_t>::max)()) {
        return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
    }
    const std::size_t query_count = query_hashes.size();
    const std::size_t directory_count = static_cast<std::size_t>(manifest->directory_entry_count);
    const std::size_t directory_byte_count = static_cast<std::size_t>(directory_bytes_u64);
    std::vector<std::byte> directory_bytes;
    std::vector<hash_major_minimizer_directory_entry> directory;
    if (directory_byte_count > directory_bytes.max_size() ||
        directory_count > directory.max_size()) {
        return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
    }
    std::uint64_t initial_logical_bytes = 0u;
    // Accumulate the first-stage logical owners without allowing byte-count overflow.
    const auto add_initial = [&initial_logical_bytes](std::uint64_t bytes) {
        return checked_add(initial_logical_bytes, bytes, initial_logical_bytes);
    };
    std::uint64_t logical = 0u;
    if (!checked_multiply(65'537u, sizeof(std::uint32_t), logical) || !add_initial(logical) ||
        !checked_multiply(query_count, sizeof(std::uint16_t), logical) || !add_initial(logical) ||
        !checked_multiply(query_count, sizeof(std::uint64_t), logical) || !add_initial(logical) ||
        !add_initial(directory_bytes_u64) ||
        !checked_multiply(directory_count, sizeof(hash_major_minimizer_directory_entry), logical) ||
        !add_initial(logical) ||
        !checked_multiply(initial_logical_bytes,
                          bioseq::parallelism::k_vector_preallocation_headroom_factor,
                          initial_logical_bytes)) {
        return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
    }
    auto initial_permit = admission.try_reserve(initial_logical_bytes);
    if (!initial_permit) {
        return std::unexpected(hash_major_minimizer_error::admission);
    }
    reader.permit_ = std::move(*initial_permit);
    reader.record_admitted_peak_(memory_admission_stage::directory, reader.permit_.bytes());
    try {
        reader.canonical_radix_prefix_.reserve(65'537u);
        reader.canonical_radix_prefix_.resize(65'537u, 0u);
        reader.primary_counts_.reserve(query_count);
        reader.primary_counts_.resize(query_count, 0u);
        reader.query_offsets_.reserve(query_count);
        reader.query_offsets_.resize(query_count, k_absent_query_offset);
        directory_bytes.reserve(directory_byte_count);
        directory_bytes.resize(directory_byte_count);
        directory.reserve(directory_count);
    } catch (const std::bad_alloc&) {
        return std::unexpected(hash_major_minimizer_error::admission);
    }
    if (!reader.refresh_persistent_owner_bytes_()) {
        return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
    }
    std::uint64_t directory_buffer_bytes = 0u;
    std::uint64_t directory_owner_bytes = 0u;
    std::uint64_t current_owner_bytes = 0u;
    if (!vector_capacity_bytes_(directory_bytes, directory_buffer_bytes) ||
        !vector_capacity_bytes_(directory, directory_owner_bytes) ||
        !checked_add(reader.persistent_owner_bytes_, directory_buffer_bytes, current_owner_bytes) ||
        !checked_add(current_owner_bytes, directory_owner_bytes, current_owner_bytes) ||
        current_owner_bytes > reader.permit_.bytes()) {
        return std::unexpected(hash_major_minimizer_error::admission);
    }
    reader.permit_.shrink_to(current_owner_bytes);
    for (std::size_t ordinal = 0u; ordinal < query_count; ++ordinal) {
        ++reader.canonical_radix_prefix_[static_cast<std::size_t>(query_hashes.at(ordinal) >> 16u) +
                                         1u];
    }
    for (std::size_t radix = 1u; radix < reader.canonical_radix_prefix_.size(); ++radix) {
        reader.canonical_radix_prefix_[radix] += reader.canonical_radix_prefix_[radix - 1u];
    }
    for (const auto& descriptor : manifest->streams) {
        if (source.size(descriptor.kind) != descriptor.logical_bytes) {
            return std::unexpected(hash_major_minimizer_error::stream_descriptor);
        }
    }
    if (!directory_bytes.empty() &&
        !source.read(hash_major_minimizer_stream_kind::directory, 0u, directory_bytes)) {
        return std::unexpected(hash_major_minimizer_error::source_read);
    }
    const std::uint64_t directory_crc = crc64_ecma182(directory_bytes);
    if (directory_crc != manifest->authenticated_directory_crc64 ||
        directory_crc != manifest->streams[0].stream_crc64) {
        return std::unexpected(hash_major_minimizer_error::checksum);
    }
    reader.memory_stats_.directory_stream_crc_receipts = 1u;
    std::uint64_t expected_control_offset = 0u;
    std::uint64_t expected_posting_offset = 0u;
    std::uint64_t expected_posting_ordinal = 0u;
    std::uint64_t observed_hashes = 0u;
    std::uint64_t observed_postings = 0u;
    for (std::size_t index = 0u; index < manifest->directory_entry_count; ++index) {
        auto entry = decode_hash_major_minimizer_directory_entry(
            std::span<const std::byte>{directory_bytes}.subspan(
                index * k_hash_major_minimizer_directory_entry_bytes,
                k_hash_major_minimizer_directory_entry_bytes));
        if (!entry)
            return std::unexpected(entry.error());
        std::uint64_t next_control = 0u;
        std::uint64_t next_posting = 0u;
        std::uint64_t next_posting_ordinal = 0u;
        std::uint64_t entry_posting_bytes = 0u;
        if (entry->control_block_ordinal != index ||
            entry->control_byte_offset != expected_control_offset ||
            entry->posting_byte_offset != expected_posting_offset ||
            entry->first_posting_ordinal != expected_posting_ordinal ||
            (index != 0u && entry->first_hash <= directory.back().last_hash) ||
            entry->posting_byte_offset % manifest->storage_profile.stored_posting_width_bytes !=
                0u ||
            entry->control_byte_length > k_hash_major_minimizer_maximum_control_block_bytes ||
            entry->last_hash > maximum_hash ||
            !checked_multiply(entry->exact_posting_count,
                              manifest->storage_profile.stored_posting_width_bytes,
                              entry_posting_bytes) ||
            entry_posting_bytes != entry->posting_byte_length ||
            !checked_add(expected_control_offset, entry->control_byte_length, next_control) ||
            !checked_add(expected_posting_offset, entry->posting_byte_length, next_posting) ||
            !checked_add(
                expected_posting_ordinal, entry->exact_posting_count, next_posting_ordinal) ||
            !checked_add(observed_hashes, entry->control_count, observed_hashes) ||
            !checked_add(observed_postings, entry->exact_posting_count, observed_postings)) {
            return std::unexpected(hash_major_minimizer_error::directory);
        }
        expected_control_offset = next_control;
        expected_posting_offset = next_posting;
        expected_posting_ordinal = next_posting_ordinal;
        directory.push_back(*entry);
    }
    if (expected_control_offset != manifest->total_control_bytes ||
        expected_posting_offset != manifest->total_posting_bytes ||
        observed_hashes != manifest->unique_hash_count ||
        observed_postings != manifest->total_posting_count) {
        return std::unexpected(hash_major_minimizer_error::directory);
    }
    {
        std::vector<std::byte> empty;
        directory_bytes.swap(empty);
    }
    if (!checked_add(reader.persistent_owner_bytes_, directory_owner_bytes, current_owner_bytes)) {
        return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
    }
    reader.permit_.shrink_to(current_owner_bytes);

    std::vector<block_authority> authorities;
    crc64_ecma182_accumulator control_stream_crc;
    std::size_t selection_query_cursor = 0u;
    std::size_t selected_block_count = 0u;
    std::uint64_t selected_control_count = 0u;
    std::uint64_t selected_posting_count = 0u;
    for (std::size_t directory_ordinal = 0u; directory_ordinal < directory.size();
         ++directory_ordinal) {
        const auto& entry = directory[directory_ordinal];
        selection_query_cursor =
            reader.canonical_lower_bound_(selection_query_cursor, entry.first_hash);
        if (selection_query_cursor == query_hashes.size())
            break;
        if (query_hashes.at(selection_query_cursor) > entry.last_hash) {
            continue;
        }
        ++selected_block_count;
        if (!checked_add(selected_control_count, entry.control_count, selected_control_count) ||
            !checked_add(
                selected_posting_count, entry.exact_posting_count, selected_posting_count)) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
    }
    reader.memory_stats_.selected_block_count = selected_block_count;
    const std::size_t exception_capacity =
        static_cast<std::size_t>(std::min({static_cast<std::uint64_t>(query_count),
                                           selected_control_count,
                                           selected_posting_count / 65'535u}));
    const std::size_t control_range_capacity =
        std::min(selected_block_count, k_hash_major_minimizer_control_read_wave_blocks);
    std::uint64_t control_vector_logical_bytes = 0u;
    std::uint64_t control_vector_bytes = 0u;
    std::uint64_t control_requested = 0u;
    if (!checked_multiply(
            selected_block_count, sizeof(block_authority), control_vector_logical_bytes) ||
        !checked_multiply(exception_capacity, sizeof(exact_global_count_exception), logical) ||
        !checked_add(control_vector_logical_bytes, logical, control_vector_logical_bytes) ||
        !checked_multiply(control_range_capacity,
                          sizeof(hash_major_minimizer_authorized_control_range),
                          logical) ||
        !checked_add(control_vector_logical_bytes, logical, control_vector_logical_bytes) ||
        !checked_multiply(control_vector_logical_bytes,
                          bioseq::parallelism::k_vector_preallocation_headroom_factor,
                          control_vector_bytes) ||
        !checked_add(
            current_owner_bytes, sizeof(hash_major_minimizer_control_scratch), control_requested) ||
        !checked_add(control_requested, control_vector_bytes, control_requested) ||
        !reader.grow_permit_to_(control_requested, memory_admission_stage::control)) {
        return std::unexpected(hash_major_minimizer_error::admission);
    }
    try {
        reader.count_exceptions_.reserve(exception_capacity);
        authorities.reserve(selected_block_count);
    } catch (const std::bad_alloc&) {
        return std::unexpected(hash_major_minimizer_error::admission);
    }
    std::uint64_t authority_owner_bytes = 0u;
    std::uint64_t control_range_owner_bytes = 0u;
    {
        hash_major_minimizer_control_scratch scratch;
        std::vector<hash_major_minimizer_authorized_control_range> control_ranges;
        try {
            control_ranges.reserve(control_range_capacity);
        } catch (const std::bad_alloc&) {
            return std::unexpected(hash_major_minimizer_error::admission);
        }
        if (!reader.refresh_persistent_owner_bytes_() ||
            !vector_capacity_bytes_(authorities, authority_owner_bytes) ||
            !vector_capacity_bytes_(control_ranges, control_range_owner_bytes) ||
            !checked_add(
                reader.persistent_owner_bytes_, directory_owner_bytes, current_owner_bytes) ||
            !checked_add(current_owner_bytes, authority_owner_bytes, current_owner_bytes) ||
            !checked_add(current_owner_bytes, control_range_owner_bytes, current_owner_bytes) ||
            !checked_add(current_owner_bytes,
                         sizeof(hash_major_minimizer_control_scratch),
                         current_owner_bytes) ||
            current_owner_bytes > reader.permit_.bytes()) {
            return std::unexpected(hash_major_minimizer_error::admission);
        }
        reader.permit_.shrink_to(current_owner_bytes);
        reader.control_wave_peak_owner_bytes_ =
            control_ranges.capacity() * sizeof(hash_major_minimizer_authorized_control_range);
        control_delivery_context delivery{
            .reader = &reader,
            .manifest = &*manifest,
            .query_hashes = query_hashes,
            .directory = &directory,
            .authorities = &authorities,
            .scratch = &scratch,
            .control_stream_crc = &control_stream_crc,
            .max_occ = max_occ,
        };
        // Deliver and authenticate one bounded batch of selected control blocks.
        auto deliver_control_wave = [&control_ranges, &delivery, &source, &reader, &scratch]()
            -> std::expected<void, hash_major_minimizer_error> {
            if (control_ranges.empty())
                return {};
            delivery.ranges = &control_ranges;
            delivery.next_range = 0u;
            bool delivered = false;
            if (source.read_control_wave != nullptr) {
                delivered = source.read_control_wave(
                    source.context, control_ranges, &delivery, &consume_control_block_);
            } else {
                ++reader.memory_stats_.generic_source_fallback_calls;
                delivered = true;
                for (std::size_t ordinal = 0u; ordinal < control_ranges.size(); ++ordinal) {
                    const auto& range = control_ranges[ordinal];
                    const auto encoded =
                        std::span<std::byte>{scratch.encoded}.first(range.byte_count);
                    if (!source.read(hash_major_minimizer_stream_kind::controls,
                                     range.control_byte_offset,
                                     encoded) ||
                        !consume_control_block_(
                            &delivery, static_cast<std::uint32_t>(ordinal), encoded)) {
                        delivered = false;
                        break;
                    }
                }
            }
            if (!delivered || delivery.error || delivery.next_range != control_ranges.size()) {
                return std::unexpected(
                    delivery.error.value_or(hash_major_minimizer_error::source_read));
            }
            std::uint64_t block_receipts = 0u;
            std::uint64_t semantic_receipts = 0u;
            if (!checked_add(reader.memory_stats_.control_block_crc_receipts,
                             control_ranges.size(),
                             block_receipts) ||
                !checked_add(reader.memory_stats_.control_semantic_receipts,
                             control_ranges.size(),
                             semantic_receipts)) {
                return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
            }
            reader.memory_stats_.control_block_crc_receipts = block_receipts;
            reader.memory_stats_.control_semantic_receipts = semantic_receipts;
            control_ranges.clear();
            return {};
        };
        std::size_t delivery_query_cursor = 0u;
        for (std::size_t directory_ordinal = 0u; directory_ordinal < directory.size();
             ++directory_ordinal) {
            const auto& entry = directory[directory_ordinal];
            delivery_query_cursor =
                reader.canonical_lower_bound_(delivery_query_cursor, entry.first_hash);
            if (delivery_query_cursor == query_count)
                break;
            if (query_hashes.at(delivery_query_cursor) > entry.last_hash) {
                continue;
            }
            control_ranges.push_back(hash_major_minimizer_authorized_control_range{
                .control_block_ordinal = directory_ordinal,
                .control_byte_offset = entry.control_byte_offset,
                .byte_count = entry.control_byte_length,
            });
            if (control_ranges.size() == k_hash_major_minimizer_control_read_wave_blocks) {
                if (auto delivered = deliver_control_wave(); !delivered) {
                    return std::unexpected(delivered.error());
                }
            }
        }
        if (auto delivered = deliver_control_wave(); !delivered) {
            return std::unexpected(delivered.error());
        }
        if (delivery.controls_read != selected_block_count) {
            return std::unexpected(hash_major_minimizer_error::source_read);
        }
        if (delivery.query_cursor > query_hashes.size()) {
            return std::unexpected(hash_major_minimizer_error::state);
        }
        if (reader.memory_stats_.control_block_crc_receipts != selected_block_count ||
            reader.memory_stats_.control_semantic_receipts != selected_block_count) {
            return std::unexpected(hash_major_minimizer_error::state);
        }
    }
    if (selected_block_count == directory.size() &&
        control_stream_crc.checksum() != manifest->streams[1].stream_crc64) {
        return std::unexpected(hash_major_minimizer_error::checksum);
    }
    {
        std::vector<hash_major_minimizer_directory_entry> empty;
        directory.swap(empty);
    }
    if (!reader.refresh_persistent_owner_bytes_() ||
        !checked_add(reader.persistent_owner_bytes_, authority_owner_bytes, current_owner_bytes)) {
        return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
    }
    reader.permit_.shrink_to(current_owner_bytes);
    std::uint64_t retained_count = 0u;
    for (std::size_t query_ordinal = 0u; query_ordinal < query_count; ++query_ordinal) {
        if (reader.query_offsets_[query_ordinal] < k_unavailable_query_offset &&
            !checked_add(retained_count, reader.exact_count_(query_ordinal), retained_count)) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
    }
    if (retained_count >= k_unavailable_query_offset ||
        retained_count > (std::numeric_limits<std::size_t>::max)()) {
        return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
    }
    reader.memory_stats_.exact_count_exception_count = reader.count_exceptions_.size();
    reader.memory_stats_.retained_position_count = retained_count;
    std::uint64_t retained_logical_bytes = 0u;
    if (authority_owner_bytes > (std::numeric_limits<std::size_t>::max)()) {
        return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
    }
    reader.transient_authority_peak_owner_bytes_ = static_cast<std::size_t>(authority_owner_bytes);

    constexpr std::size_t posting_wave_owner_ceiling = k_hash_major_minimizer_read_slab_bytes;
    constexpr std::size_t bytes_per_worst_case_request =
        sizeof(posting_request) + sizeof(hash_major_minimizer_authorized_posting_range);
    const std::size_t wave_request_capacity =
        std::min(k_hash_major_minimizer_posting_request_wave_hashes,
                 posting_wave_owner_ceiling / bytes_per_worst_case_request);
    static_assert(k_hash_major_minimizer_controls_per_block <
                  posting_wave_owner_ceiling / bytes_per_worst_case_request);
    const std::size_t accepted_hash_count =
        static_cast<std::size_t>(reader.memory_stats_.accepted_hash_count);
    const std::size_t requested_wave_capacity =
        std::min(accepted_hash_count, wave_request_capacity);
    std::vector<posting_request> requests;
    std::vector<hash_major_minimizer_authorized_posting_range> ranges;
    std::unique_ptr<std::byte[]> fallback_slab;
    const std::uint64_t fallback_slab_bytes =
        source.read_posting_wave == nullptr && accepted_hash_count != 0u
            ? k_hash_major_minimizer_read_slab_bytes
            : 0u;
    std::uint64_t posting_vector_logical_bytes = 0u;
    std::uint64_t posting_vector_preallocation_bytes = 0u;
    std::uint64_t posting_requested = 0u;
    if (!checked_multiply(retained_count,
                          manifest->storage_profile.stored_posting_width_bytes,
                          retained_logical_bytes) ||
        !checked_multiply(
            requested_wave_capacity, bytes_per_worst_case_request, posting_vector_logical_bytes) ||
        !checked_multiply(posting_vector_logical_bytes,
                          bioseq::parallelism::k_vector_preallocation_headroom_factor,
                          posting_vector_preallocation_bytes) ||
        !checked_add(current_owner_bytes, retained_logical_bytes, posting_requested) ||
        !checked_add(posting_requested, posting_vector_preallocation_bytes, posting_requested) ||
        !checked_add(posting_requested, fallback_slab_bytes, posting_requested) ||
        !reader.grow_permit_to_(posting_requested, memory_admission_stage::posting)) {
        return std::unexpected(hash_major_minimizer_error::admission);
    }
    try {
        if (retained_count != 0u) {
            if (manifest->storage_profile.stored_posting_width_bytes == sizeof(std::uint32_t)) {
                reader.retained_positions32_ = std::make_unique_for_overwrite<std::uint32_t[]>(
                    static_cast<std::size_t>(retained_count));
            } else {
                reader.retained_positions64_ = std::make_unique_for_overwrite<std::uint64_t[]>(
                    static_cast<std::size_t>(retained_count));
            }
        }
        reader.retained_position_count_ = static_cast<std::size_t>(retained_count);
        requests.reserve(requested_wave_capacity);
        ranges.reserve(requested_wave_capacity);
        if (fallback_slab_bytes != 0u) {
            fallback_slab =
                std::make_unique_for_overwrite<std::byte[]>(k_hash_major_minimizer_read_slab_bytes);
        }
    } catch (const std::bad_alloc&) {
        return std::unexpected(hash_major_minimizer_error::admission);
    }
    std::uint64_t request_capacity_bytes = 0u;
    std::uint64_t range_capacity_bytes = 0u;
    std::uint64_t wave_capacity_bytes = 0u;
    if (!checked_multiply(requests.capacity(), sizeof(posting_request), request_capacity_bytes) ||
        !checked_multiply(ranges.capacity(),
                          sizeof(hash_major_minimizer_authorized_posting_range),
                          range_capacity_bytes) ||
        !checked_add(request_capacity_bytes, range_capacity_bytes, wave_capacity_bytes) ||
        wave_capacity_bytes > posting_wave_owner_ceiling ||
        !reader.refresh_persistent_owner_bytes_() ||
        !checked_add(reader.persistent_owner_bytes_, authority_owner_bytes, current_owner_bytes) ||
        !checked_add(current_owner_bytes, wave_capacity_bytes, current_owner_bytes) ||
        !checked_add(current_owner_bytes, fallback_slab_bytes, current_owner_bytes) ||
        current_owner_bytes > reader.permit_.bytes()) {
        return std::unexpected(hash_major_minimizer_error::admission);
    }
    reader.permit_.shrink_to(current_owner_bytes);
    reader.posting_wave_peak_owner_bytes_ = static_cast<std::size_t>(wave_capacity_bytes);
    crc64_ecma182_accumulator posting_stream_crc;
    std::uint64_t posting_stream_next = 0u;
    bool posting_stream_complete = true;
    std::size_t query_cursor = 0u;
    std::size_t authority_cursor = 0u;
    std::size_t accepted_delivered = 0u;
    std::uint64_t output_cursor = 0u;
    // Skip prepared hashes whose posting count exceeded the retention threshold.
    const auto next_accepted_query = [&reader, query_count](std::size_t ordinal) noexcept {
        while (ordinal < query_count &&
               reader.query_offsets_[ordinal] >= k_unavailable_query_offset) {
            ++ordinal;
        }
        return ordinal;
    };
    while (accepted_delivered < accepted_hash_count) {
        requests.clear();
        ranges.clear();
        const std::size_t wave_accepted_begin = accepted_delivered;
        while (authority_cursor < authorities.size()) {
            const block_authority& authority = authorities[authority_cursor];
            std::uint64_t authority_end = 0u;
            if (!checked_add(
                    authority.posting_byte_offset, authority.posting_byte_length, authority_end)) {
                return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
            }
            query_cursor = next_accepted_query(query_cursor);
            if (query_cursor == query_count)
                break;
            if (reader.query_offsets_[query_cursor] >= authority_end) {
                ++authority_cursor;
                continue;
            }
            if (reader.query_offsets_[query_cursor] < authority.posting_byte_offset) {
                return std::unexpected(hash_major_minimizer_error::posting);
            }
            std::size_t scan_query = query_cursor;
            std::size_t block_requests = 0u;
            for (;;) {
                scan_query = next_accepted_query(scan_query);
                if (scan_query == query_count ||
                    reader.query_offsets_[scan_query] >= authority_end) {
                    break;
                }
                if (reader.query_offsets_[scan_query] < authority.posting_byte_offset) {
                    return std::unexpected(hash_major_minimizer_error::posting);
                }
                ++block_requests;
                ++scan_query;
            }
            const std::size_t block_next_query = scan_query;
            if (block_requests == 0u) {
                ++authority_cursor;
                continue;
            }
            if (!requests.empty() && requests.size() + block_requests > wave_request_capacity) {
                break;
            }
            if (requests.size() + block_requests > wave_request_capacity) {
                return std::unexpected(hash_major_minimizer_error::state);
            }
            const std::size_t block_request_begin = requests.size();
            for (std::size_t query_ordinal = query_cursor; query_ordinal < block_next_query;
                 ++query_ordinal) {
                const std::uint64_t posting_offset = reader.query_offsets_[query_ordinal];
                if (posting_offset >= k_unavailable_query_offset)
                    continue;
                const std::uint64_t count = reader.exact_count_(query_ordinal);
                std::uint64_t posting_bytes = 0u;
                std::uint64_t posting_end = 0u;
                if (count == 0u ||
                    !checked_multiply(count,
                                      manifest->storage_profile.stored_posting_width_bytes,
                                      posting_bytes) ||
                    !checked_add(posting_offset, posting_bytes, posting_end) ||
                    posting_offset < authority.posting_byte_offset || posting_end > authority_end) {
                    return std::unexpected(count == 0u ? hash_major_minimizer_error::count
                                                       : hash_major_minimizer_error::posting);
                }
                requests.push_back(posting_request{
                    .posting_byte_offset = posting_offset,
                    .exact_count = count,
                    .query_ordinal = static_cast<std::uint32_t>(query_ordinal),
                    .flags = 0u,
                });
                reader.query_offsets_[query_ordinal] = output_cursor;
                if (!checked_add(output_cursor, count, output_cursor)) {
                    return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
                }
            }
            const std::size_t block_request_end = requests.size();
            if (block_request_end - block_request_begin != block_requests) {
                return std::unexpected(hash_major_minimizer_error::state);
            }
            for (std::size_t begin = block_request_begin; begin < block_request_end;) {
                std::uint64_t first_request_bytes = 0u;
                std::uint64_t range_end = 0u;
                if (!checked_multiply(requests[begin].exact_count,
                                      manifest->storage_profile.stored_posting_width_bytes,
                                      first_request_bytes) ||
                    !checked_add(
                        requests[begin].posting_byte_offset, first_request_bytes, range_end)) {
                    return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
                }
                std::size_t end = begin + 1u;
                while (end < block_request_end && requests[end].posting_byte_offset == range_end) {
                    std::uint64_t next_request_bytes = 0u;
                    if (!checked_multiply(requests[end].exact_count,
                                          manifest->storage_profile.stored_posting_width_bytes,
                                          next_request_bytes) ||
                        !checked_add(range_end, next_request_bytes, range_end)) {
                        return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
                    }
                    ++end;
                }
                if (requests[begin].posting_byte_offset < authority.posting_byte_offset ||
                    range_end > authority_end) {
                    return std::unexpected(hash_major_minimizer_error::posting);
                }
                const bool complete_block =
                    block_request_begin == begin && end == block_request_end &&
                    requests[begin].posting_byte_offset == authority.posting_byte_offset &&
                    range_end == authority_end;
                ranges.push_back(hash_major_minimizer_authorized_posting_range{
                    .control_block_ordinal = authority.directory_ordinal,
                    .posting_byte_offset = requests[begin].posting_byte_offset,
                    .byte_count = range_end - requests[begin].posting_byte_offset,
                    .expected_posting_crc64 = authority.posting_crc64,
                    .request_begin = static_cast<std::uint32_t>(begin),
                    .request_count = static_cast<std::uint32_t>(end - begin),
                    .flags = complete_block ? 1u : 0u,
                });
                begin = end;
            }
            accepted_delivered += block_requests;
            query_cursor = block_next_query;
            ++authority_cursor;
        }
        if (requests.empty() || ranges.empty() || requests.size() > wave_request_capacity) {
            return std::unexpected(hash_major_minimizer_error::state);
        }
        posting_delivery_context delivery{
            .reader = &reader,
            .manifest = &*manifest,
            .requests = &requests,
            .ranges = &ranges,
            .next_request = ranges.front().request_begin,
            .posting_stream_crc = &posting_stream_crc,
            .posting_stream_next = &posting_stream_next,
            .posting_stream_complete = &posting_stream_complete,
        };
        bool delivered = false;
        if (source.read_posting_wave != nullptr) {
            delivered = source.read_posting_wave(
                source.context, ranges, &delivery, &consume_posting_bytes_);
        } else {
            ++reader.memory_stats_.generic_source_fallback_calls;
            delivered = true;
            for (std::size_t range_ordinal = 0u; delivered && range_ordinal < ranges.size();
                 ++range_ordinal) {
                const auto& range = ranges[range_ordinal];
                std::uint64_t relative = 0u;
                while (relative < range.byte_count) {
                    const std::size_t bytes = static_cast<std::size_t>(std::min<std::uint64_t>(
                        k_hash_major_minimizer_read_slab_bytes, range.byte_count - relative));
                    const auto slab = std::span<std::byte>{fallback_slab.get(),
                                                           k_hash_major_minimizer_read_slab_bytes}
                                          .first(bytes);
                    if (!source.read(hash_major_minimizer_stream_kind::postings,
                                     range.posting_byte_offset + relative,
                                     slab) ||
                        !consume_posting_bytes_(
                            &delivery, static_cast<std::uint32_t>(range_ordinal), relative, slab)) {
                        delivered = false;
                        break;
                    }
                    relative += bytes;
                }
            }
        }
        if (!delivered || delivery.error || delivery.next_range != ranges.size() ||
            delivery.next_request != requests.size()) {
            return std::unexpected(
                delivery.error.value_or(hash_major_minimizer_error::source_read));
        }
        std::uint64_t delivered_range_bytes = 0u;
        for (const auto& range : ranges) {
            if (!checked_add(delivered_range_bytes, range.byte_count, delivered_range_bytes)) {
                return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
            }
        }
        if (!checked_add(reader.memory_stats_.authorized_posting_bytes,
                         delivered_range_bytes,
                         delivered_range_bytes)) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
        reader.memory_stats_.authorized_posting_bytes = delivered_range_bytes;
        if (accepted_delivered == wave_accepted_begin) {
            return std::unexpected(hash_major_minimizer_error::state);
        }
    }
    if (accepted_delivered != accepted_hash_count || output_cursor != retained_count) {
        return std::unexpected(hash_major_minimizer_error::state);
    }
    std::vector<posting_request>{}.swap(requests);
    std::vector<hash_major_minimizer_authorized_posting_range>{}.swap(ranges);
    fallback_slab.reset();
    std::vector<block_authority>{}.swap(authorities);
    if (posting_stream_complete && posting_stream_next == manifest->total_posting_bytes &&
        posting_stream_crc.checksum() != manifest->streams[2].stream_crc64) {
        return std::unexpected(hash_major_minimizer_error::checksum);
    }
    if (!reader.refresh_persistent_owner_bytes_()) {
        return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
    }
    reader.permit_.shrink_to(reader.persistent_owner_bytes_);
    reader.memory_stats_.retained_owner_bytes = reader.persistent_owner_bytes_;
    return reader;
}

} // namespace bioseq::indexing
