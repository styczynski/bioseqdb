/**
 * @file compact_direct/reader/opening.cppm
 * @brief Compact-direct and grouped reader validation, admission, and opening.
 */

export module bioseq.indexing.minimizer_hashmap:compact_direct.reader_opening;

import :compact_direct.reader_contract;
import std;
import :exact_simd;
import :grouped_soa;
import :prepared_reader;
import :compact_direct.query_contracts;
import :compact_direct.planning_admission;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.common.checked_arithmetic;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.chaining.core;
import bioseq.common.radix_sort;
import bioseq.instrumentation;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;

export namespace bioseq::indexing {

/**
 * Validate grouped-SoA segment-local profile geometry, generation identity, and compact-direct image
 * against the expected storage contract.
 * Returns profile and hash-domain bit width when layout, max_occ, checksums, and direct header geometry
 * match the manifest and owned image span.
 * Otherwise returns storage_contract_mismatch, identity_mismatch, checksum, or the storage-contract
 * validation error.
 */
[[nodiscard]] inline std::expected<prepared_minimizer_fragment_reader::direct_open_contract,
                                   minimizer_generation_encoding_error>
prepared_minimizer_fragment_reader::validate_direct_open_contract_(
    const minimizer_generation_head& head,
    const minimizer_generation_manifest& manifest,
    const expected_minimizer_generation_identity& expected,
    std::uint64_t max_occ,
    const owned_compact_hash32_direct_index_view& compact_direct) {
    const auto profile = derive_grouped_soa_minimizer_profile(manifest);
    if (!profile || manifest.layout != minimizer_generation_layout::grouped_soa_segment_local ||
        head.layout != manifest.layout || expected.layout != manifest.layout ||
        !expected.max_occ_cap || *expected.max_occ_cap != profile->max_occ_cap ||
        max_occ > profile->max_occ_cap ||
        profile->max_hash > (std::numeric_limits<std::uint32_t>::max)() ||
        max_occ > static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    if (auto validated = validate_minimizer_generation_storage_contract(manifest, expected);
        !validated) {
        return std::unexpected(validated.error());
    }

    const std::uint32_t profile_max_hash = static_cast<std::uint32_t>(profile->max_hash);
    const std::uint32_t profile_hash_bits = std::bit_width(profile_max_hash);
    const std::uint64_t expected_profile_max_hash =
        profile_hash_bits == 0u ? 0u : (std::uint64_t{1} << profile_hash_bits) - 1u;
    if (profile_hash_bits == 0u || profile_hash_bits > (std::numeric_limits<std::uint8_t>::max)() ||
        profile->max_hash != expected_profile_max_hash) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }

    if (head.generation_id != manifest.generation_id ||
        head.source_identity != manifest.source_identity ||
        head.source_content_checksum != manifest.source_content_checksum) {
        return std::unexpected(minimizer_generation_encoding_error::identity_mismatch);
    }
    const auto manifest_bytes = encode_minimizer_generation_manifest_record(manifest);
    if (!manifest_bytes) {
        return std::unexpected(manifest_bytes.error());
    }
    if (head.manifest_checksum != crc64_ecma182(std::span<const std::byte>{*manifest_bytes})) {
        return std::unexpected(minimizer_generation_encoding_error::checksum);
    }
    if (manifest.gpu_abi_revision != profile->gpu_abi_revision ||
        head.storage_contract_checksum !=
            compute_minimizer_generation_storage_contract_checksum(manifest)) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }

    const compact_hash32_header& direct_header = compact_direct.view_.header;
    const direct_index_identity expected_identity{
        .generation_id = head.generation_id,
        .source_identity = head.source_identity,
        .storage_contract_checksum = head.storage_contract_checksum,
        .target_length = manifest.target_length,
    };
    if (direct_header.identity != expected_identity ||
        direct_header.max_occ_cap != profile->max_occ_cap ||
        direct_header.geometry.hash_domain_bits != profile_hash_bits ||
        direct_header.geometry.max_hash != profile_max_hash ||
        direct_header.image_bytes != compact_direct.view_.image.size() ||
        compact_direct.view_.image.empty() ||
        compact_direct.image_owner_.get() != compact_direct.view_.image.data()) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    return direct_open_contract{
        .profile = *profile,
        .hash_domain_bits = static_cast<std::uint8_t>(profile_hash_bits),
    };
}

[[nodiscard]] inline std::expected<prepared_minimizer_fragment_reader,
                                   minimizer_generation_encoding_error>
prepared_minimizer_fragment_reader::open_compact_direct(
    bioseq::parallelism::memory_admission_controller& admission,
    const minimizer_generation_head& head,
    const minimizer_generation_manifest& manifest,
    const expected_minimizer_generation_identity& expected,
    std::uint64_t max_occ,
    owned_compact_hash32_direct_index_view compact_direct,
    compact_direct_selected_page_adviser selected_page_adviser) {
    if (!selected_page_adviser.empty() && !selected_page_adviser.valid()) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    const auto contract =
        validate_direct_open_contract_(head, manifest, expected, max_occ, compact_direct);
    if (!contract) {
        return std::unexpected(contract.error());
    }

    auto permit = admission.try_reserve(0u);
    if (!permit) {
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }

    prepared_minimizer_fragment_reader reader;
    reader.permit_ = std::move(*permit);
    reader.head_ = head;
    reader.manifest_ = manifest;
    reader.profile_ = contract->profile;
    reader.max_occ_ = max_occ;
    reader.backing_kind_ = backing_kind::compact_direct_only;
    reader.compact_direct_ = std::move(compact_direct);
    reader.selected_page_adviser_ = selected_page_adviser;
    reader.query_radix_shift_ =
        static_cast<std::uint8_t>(contract->hash_domain_bits > query_radix_bits_
                                      ? contract->hash_domain_bits - query_radix_bits_
                                      : 0u);
    reader.query_membership_filter_shift_ = static_cast<std::uint8_t>(
        contract->hash_domain_bits > 21u ? contract->hash_domain_bits - 21u : 0u);
    reader.query_membership_filter_mask_id_mask_ =
        reader.query_membership_filter_shift_ == 0u
            ? 0u
            : static_cast<std::uint16_t>(
                  (std::uint32_t{1} << reader.query_membership_filter_shift_) - 1u);
    reader.wire_dispatch_ = reader.profile_.hash_width == minimizer_scalar_width::u32 &&
                                    reader.profile_.count_width == minimizer_scalar_width::u16 &&
                                    reader.profile_.position_width == minimizer_scalar_width::u16
                                ? fragment_encoding_dispatch::h32_c16_p16
                                : fragment_encoding_dispatch::checked_dynamic;

    // validate_direct_open_contract_ has already authenticated the
    // manifest checksum and its standalone uniform-segment equations.
    // Compact-direct queries address postings through the direct image,
    // so re-deriving every segment core and retaining fragment_block_plan
    // objects would be redundant O(segment_count) cold-open work.

    reader.ready_ = true;
    return reader;
}

/**
 * Open a grouped-SoA minimizer fragment reader over a mapped payload and encoded directory.
 * Optionally attaches a compact-direct index view validated with the same identity and geometry checks
 * as validate_direct_open_contract_.
 * Decodes the directory under admission, builds per-entry control and position address plans, and packs
 * fragment block plans that never split a position-segment run.
 * Returns admission, arithmetic_overflow, directory_size, position_segment, byte_count, bp_range,
 * directory_order, or decode errors when mapping sizes, payload layout, or segment ordering disagree.
 */
[[nodiscard]] inline std::expected<prepared_minimizer_fragment_reader,
                                   minimizer_generation_encoding_error>
prepared_minimizer_fragment_reader::open(
    bioseq::parallelism::memory_admission_controller& admission,
    const minimizer_generation_head& head,
    const minimizer_generation_manifest& manifest,
    const expected_minimizer_generation_identity& expected,
    std::span<const std::byte> encoded_directory,
    std::uint64_t max_occ,
    mapped_payload_owner_type mapped_payload_owner,
    std::span<const std::byte> mapped_payload,
    std::optional<owned_compact_hash32_direct_index_view> compact_direct) {
    using bioseq::common::saturating_add;
    using bioseq::common::saturating_mul;
    const auto profile = derive_grouped_soa_minimizer_profile(manifest);
    if (!profile || manifest.layout != minimizer_generation_layout::grouped_soa_segment_local ||
        head.layout != manifest.layout || expected.layout != manifest.layout ||
        !expected.max_occ_cap || *expected.max_occ_cap != profile->max_occ_cap ||
        max_occ > profile->max_occ_cap ||
        profile->max_hash > (std::numeric_limits<std::uint32_t>::max)() ||
        max_occ > static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()) ||
        mapped_payload_owner == nullptr || mapped_payload.size() != manifest.payload_bytes ||
        encoded_directory.size() != manifest.directory_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    if (auto validated = validate_minimizer_generation_storage_contract(manifest, expected);
        !validated) {
        return std::unexpected(validated.error());
    }
    const std::uint32_t profile_max_hash = static_cast<std::uint32_t>(profile->max_hash);
    const std::uint32_t profile_hash_bits = std::bit_width(profile_max_hash);
    const std::uint64_t expected_profile_max_hash =
        profile_hash_bits == 0u ? 0u : (std::uint64_t{1} << profile_hash_bits) - 1u;
    if (profile_hash_bits == 0u || profile->max_hash != expected_profile_max_hash) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    if (compact_direct) {
        const compact_hash32_header& direct_header = compact_direct->view_.header;
        const direct_index_identity expected_identity{
            .generation_id = head.generation_id,
            .source_identity = head.source_identity,
            .storage_contract_checksum = head.storage_contract_checksum,
            .target_length = manifest.target_length,
        };
        if (direct_header.identity != expected_identity ||
            direct_header.max_occ_cap != profile->max_occ_cap ||
            direct_header.geometry.hash_domain_bits != profile_hash_bits ||
            direct_header.geometry.max_hash != profile_max_hash ||
            direct_header.image_bytes != compact_direct->view_.image.size() ||
            compact_direct->view_.image.empty() ||
            compact_direct->image_owner_.get() != compact_direct->view_.image.data()) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
    }

    const std::size_t directory_stride = minimizer_directory_entry_encoded_bytes(*profile);
    if (directory_stride == 0u || encoded_directory.size() % directory_stride != 0u) {
        return std::unexpected(minimizer_generation_encoding_error::directory_size);
    }
    const std::size_t entry_count = encoded_directory.size() / directory_stride;
    if (entry_count > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::directory_size);
    }
    std::uint64_t initial_bytes =
        saturating_add(encoded_directory.size(),
                       saturating_mul(entry_count, sizeof(grouped_soa_bucket_directory_entry)));
    initial_bytes = saturating_add(initial_bytes, saturating_mul(entry_count, sizeof(entry_plan)));
    initial_bytes =
        saturating_add(initial_bytes, saturating_mul(entry_count, sizeof(fragment_block_plan)));
    if (initial_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    auto permit = admission.try_reserve(initial_bytes);
    if (!permit) {
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }

    prepared_minimizer_fragment_reader reader;
    reader.permit_ = std::move(*permit);
    reader.head_ = head;
    reader.manifest_ = manifest;
    reader.profile_ = *profile;
    reader.query_radix_shift_ = static_cast<std::uint8_t>(
        profile_hash_bits > query_radix_bits_ ? profile_hash_bits - query_radix_bits_ : 0u);
    reader.query_membership_filter_shift_ =
        static_cast<std::uint8_t>(profile_hash_bits > 21u ? profile_hash_bits - 21u : 0u);
    reader.query_membership_filter_mask_id_mask_ =
        reader.query_membership_filter_shift_ == 0u
            ? 0u
            : static_cast<std::uint16_t>(
                  (std::uint32_t{1} << reader.query_membership_filter_shift_) - 1u);
    reader.mapped_payload_owner_ = std::move(mapped_payload_owner);
    reader.mapped_payload_ = mapped_payload;
    reader.compact_direct_ = std::move(compact_direct);
    reader.entry_count_ = entry_count;
    reader.max_occ_ = max_occ;
    try {
        auto directory = grouped_soa_directory_view::decode_admitted(
            encoded_directory, *profile, reader.permit_);
        if (!directory) {
            return std::unexpected(directory.error());
        }
        if (auto metadata = validate_minimizer_generation_metadata(head, manifest, *directory);
            !metadata) {
            return std::unexpected(metadata.error());
        }
        reader.directory_ = std::move(*directory);
        if (entry_count != 0u) {
            reader.entry_plans_ = std::make_unique<entry_plan[]>(entry_count);
        }
    } catch (const std::bad_alloc&) {
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }

    std::uint64_t total_control_bytes = 0u;
    std::uint64_t total_control_hash_scan_bytes = 0u;
    std::uint64_t total_control_count_scan_bytes = 0u;
    std::uint64_t total_position_bytes = 0u;
    const auto entries = reader.directory_.entries();
    for (std::size_t ordinal = 0u; ordinal < entries.size(); ++ordinal) {
        const grouped_soa_bucket_directory_entry& entry = entries[ordinal];
        if (entry.position_segment < 0 ||
            static_cast<std::uint32_t>(entry.position_segment) >= manifest.position_segment_count) {
            return std::unexpected(minimizer_generation_encoding_error::position_segment);
        }

        if (!is_minimizer_scalar_width(profile->hash_width) ||
            !is_minimizer_scalar_width(profile->count_width) ||
            !is_minimizer_scalar_width(profile->position_width) ||
            !is_minimizer_scalar_width(entry.shape_offset_width) ||
            entry.control_element_count == 0u) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        const auto layout = plan_grouped_soa_control_plane(entry.control_element_count,
                                                           profile->hash_width,
                                                           entry.shape_offset_width,
                                                           profile->count_width);
        if (!layout || layout->encoded_bytes != entry.control_encoded_bytes) {
            return std::unexpected(layout ? minimizer_generation_encoding_error::byte_count
                                          : layout.error());
        }

        const std::uint64_t h = static_cast<std::uint8_t>(profile->hash_width);
        const std::uint64_t o = static_cast<std::uint8_t>(entry.shape_offset_width);
        const std::uint64_t c = static_cast<std::uint8_t>(profile->count_width);
        const std::uint64_t p = static_cast<std::uint8_t>(profile->position_width);
        const std::uint64_t prefix_element_count = saturating_add(entry.control_element_count, 1u);
        const std::uint64_t hash_bytes = saturating_mul(entry.control_element_count, h);
        const std::uint64_t prefix_bytes = saturating_mul(prefix_element_count, o);
        const std::uint64_t count_bytes = saturating_mul(entry.control_element_count, c);
        const std::uint64_t position_bytes = saturating_mul(entry.position_element_count, p);
        const std::uint64_t count_scan_bytes = saturating_mul(entry.control_element_count, h + c);
        if (prefix_element_count == (std::numeric_limits<std::uint64_t>::max)() ||
            hash_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            prefix_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            count_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            position_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            count_scan_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        if (position_bytes != entry.position_encoded_bytes) {
            return std::unexpected(minimizer_generation_encoding_error::byte_count);
        }

        entry_plan& plan = reader.entry_plans_[ordinal];
        plan.control_begin = total_control_bytes;
        plan.control_end = saturating_add(plan.control_begin, entry.control_encoded_bytes);
        plan.hash_begin = saturating_add(plan.control_begin, layout->hash_offset);
        plan.hash_end = saturating_add(plan.hash_begin, hash_bytes);
        plan.prefix_begin = saturating_add(plan.control_begin, layout->prefix_offset);
        plan.prefix_end = saturating_add(plan.prefix_begin, prefix_bytes);
        plan.count_begin = saturating_add(plan.control_begin, layout->count_offset);
        plan.count_end = saturating_add(plan.count_begin, count_bytes);
        if (plan.control_end == (std::numeric_limits<std::uint64_t>::max)() ||
            plan.hash_begin == (std::numeric_limits<std::uint64_t>::max)() ||
            plan.hash_end == (std::numeric_limits<std::uint64_t>::max)() ||
            plan.prefix_begin == (std::numeric_limits<std::uint64_t>::max)() ||
            plan.prefix_end == (std::numeric_limits<std::uint64_t>::max)() ||
            plan.count_begin == (std::numeric_limits<std::uint64_t>::max)() ||
            plan.count_end == (std::numeric_limits<std::uint64_t>::max)() ||
            plan.control_end > manifest.payload_bytes || plan.hash_begin != plan.control_begin ||
            plan.hash_end > plan.prefix_begin || plan.prefix_end > plan.count_begin ||
            plan.count_end > plan.control_end) {
            return std::unexpected(minimizer_generation_encoding_error::byte_count);
        }

        const auto core = derive_minimizer_position_segment_bounds(
            manifest, static_cast<std::uint32_t>(entry.position_segment));
        if (!core || core->end_bp <= core->begin_bp || core->end_bp > manifest.target_length) {
            return std::unexpected(minimizer_generation_encoding_error::bp_range);
        }
        plan.control_count = entry.control_element_count;
        plan.position_element_count = entry.position_element_count;
        plan.reference_begin = core->begin_bp;
        plan.reference_end = core->end_bp;
        plan.control_scan_bytes = count_scan_bytes;
        plan.position_segment = static_cast<std::uint32_t>(entry.position_segment);
        plan.hash_byte_width = static_cast<std::uint8_t>(profile->hash_width);
        plan.count_byte_width = static_cast<std::uint8_t>(profile->count_width);
        plan.position_byte_width = static_cast<std::uint8_t>(profile->position_width);
        plan.offset_byte_width = static_cast<std::uint8_t>(entry.shape_offset_width);
        plan.hash_width = profile->hash_width;
        plan.count_width = profile->count_width;
        plan.position_width = profile->position_width;
        plan.shape_offset_width = entry.shape_offset_width;

        total_control_bytes = plan.control_end;
        if (!add_checked_(total_control_hash_scan_bytes, hash_bytes) ||
            !add_checked_(total_control_count_scan_bytes, count_scan_bytes) ||
            !add_checked_(total_position_bytes, entry.position_encoded_bytes)) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
    }
    if (total_position_bytes > manifest.payload_bytes ||
        total_control_bytes > manifest.payload_bytes - total_position_bytes ||
        total_control_bytes + total_position_bytes != manifest.payload_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::byte_count);
    }
    reader.total_control_hash_scan_bytes_ = total_control_hash_scan_bytes;
    reader.total_control_count_scan_bytes_ = total_control_count_scan_bytes;
    std::uint64_t position_cursor = 0u;
    for (std::size_t ordinal = 0u; ordinal < entries.size(); ++ordinal) {
        entry_plan& plan = reader.entry_plans_[ordinal];
        plan.position_begin = saturating_add(total_control_bytes, position_cursor);
        plan.position_end =
            saturating_add(plan.position_begin, entries[ordinal].position_encoded_bytes);
        if (plan.position_begin == (std::numeric_limits<std::uint64_t>::max)() ||
            plan.position_end == (std::numeric_limits<std::uint64_t>::max)() ||
            plan.position_end > manifest.payload_bytes) {
            return std::unexpected(minimizer_generation_encoding_error::byte_count);
        }
        position_cursor = saturating_add(position_cursor, entries[ordinal].position_encoded_bytes);
        if (position_cursor == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
    }
    if (position_cursor != total_position_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::byte_count);
    }

    reader.wire_dispatch_ = profile->hash_width == minimizer_scalar_width::u32 &&
                                    profile->count_width == minimizer_scalar_width::u16 &&
                                    profile->position_width == minimizer_scalar_width::u16
                                ? fragment_encoding_dispatch::h32_c16_p16
                                : fragment_encoding_dispatch::checked_dynamic;

    // Build a compact address plan whose block boundaries can never split
    // an equal-position-segment run. Empty physical segments are permitted
    // gaps and need no synthetic directory entry.
    constexpr std::size_t target_entries_per_block = 128u;
    for (std::size_t ordinal = 1u; ordinal < entries.size(); ++ordinal) {
        if (entries[ordinal].position_segment < entries[ordinal - 1u].position_segment) {
            return std::unexpected(minimizer_generation_encoding_error::directory_order);
        }
    }
    std::size_t block_count = 0u;
    for (std::size_t begin = 0u; begin < entries.size();) {
        std::size_t end = std::min(entries.size(), begin + target_entries_per_block);
        const auto terminal_segment = entries[end - 1u].position_segment;
        while (end < entries.size() && entries[end].position_segment == terminal_segment) {
            ++end;
        }
        ++block_count;
        begin = end;
    }
    try {
        if (block_count != 0u) {
            reader.block_plans_ = std::make_unique<fragment_block_plan[]>(block_count);
        }
    } catch (const std::bad_alloc&) {
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }
    reader.block_count_ = block_count;

    std::uint64_t previous_block_reference_end = 0u;
    bool have_previous_block = false;
    std::size_t block_ordinal = 0u;
    for (std::size_t begin = 0u; begin < entries.size();) {
        std::size_t end = std::min(entries.size(), begin + target_entries_per_block);
        const auto terminal_segment = entries[end - 1u].position_segment;
        while (end < entries.size() && entries[end].position_segment == terminal_segment) {
            ++end;
        }
        if (begin >= end || end > entries.size() ||
            end > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::directory_size);
        }

        fragment_block_plan& block = reader.block_plans_[block_ordinal];
        block.entry_begin = static_cast<std::uint32_t>(begin);
        block.entry_end = static_cast<std::uint32_t>(end);
        block.first_position_segment = static_cast<std::uint32_t>(entries[begin].position_segment);
        block.last_position_segment =
            static_cast<std::uint32_t>(entries[end - 1u].position_segment);

        bool have_core = false;
        std::uint64_t previous_core_end = 0u;
        std::int32_t previous_segment = -1;
        std::size_t segment_run_begin = begin;
        for (std::size_t ordinal = begin; ordinal < end; ++ordinal) {
            const std::int32_t segment = entries[ordinal].position_segment;
            if (segment == previous_segment)
                continue;
            if (previous_segment >= 0 && ordinal - segment_run_begin > target_entries_per_block) {
                ++reader.oversized_segment_block_count_;
            }
            segment_run_begin = ordinal;
            const entry_plan& planned_entry = reader.entry_plans_[ordinal];
            if (planned_entry.position_segment != static_cast<std::uint32_t>(segment) ||
                planned_entry.reference_end <= planned_entry.reference_begin ||
                (have_core && previous_core_end > planned_entry.reference_begin)) {
                return std::unexpected(minimizer_generation_encoding_error::bp_range);
            }
            if (!have_core) {
                block.reference_begin = planned_entry.reference_begin;
                have_core = true;
            }
            block.reference_end = planned_entry.reference_end;
            previous_core_end = planned_entry.reference_end;
            previous_segment = segment;
        }
        if (end - segment_run_begin > target_entries_per_block) {
            ++reader.oversized_segment_block_count_;
        }
        if (!have_core || block.reference_end <= block.reference_begin ||
            (have_previous_block && previous_block_reference_end > block.reference_begin)) {
            return std::unexpected(minimizer_generation_encoding_error::bp_range);
        }
        previous_block_reference_end = block.reference_end;
        have_previous_block = true;
        reader.largest_block_entry_count_ =
            std::max(reader.largest_block_entry_count_, static_cast<std::uint32_t>(end - begin));
        ++block_ordinal;
        begin = end;
    }
    if (block_ordinal != block_count) {
        return std::unexpected(minimizer_generation_encoding_error::directory_size);
    }

    // The PostgreSQL-owned image is published only after a complete
    // address-bound row and fragment-extent CRC reread, fsync, and durable
    // rename. Its exact generation header was checked before this call.
    // Trust those immutable fragment payloads here. Reader open owns only
    // directory/address plans; max-occ reduction is fused into the later
    // fragment-parallel query wave so minimizer-fragment has no target-wide open scan.

    reader.owned_bytes_ = reader.final_owned_bytes_();
    if (reader.owned_bytes_ == (std::numeric_limits<std::uint64_t>::max)() ||
        (reader.owned_bytes_ > reader.permit_.bytes() &&
         !reader.permit_.grow_to(reader.owned_bytes_))) {
        return std::unexpected(reader.owned_bytes_ == (std::numeric_limits<std::uint64_t>::max)()
                                   ? minimizer_generation_encoding_error::arithmetic_overflow
                                   : minimizer_generation_encoding_error::admission);
    }
    reader.permit_.shrink_to(reader.owned_bytes_);
    reader.ready_ = true;
    return reader;
}

/**
 * Report open-side retained capacity for the decoded directory, entry plans, and fragment block plans.
 * Query-wave counters and payload span fields stay zero until prepare_fragment_anchor_batch runs.
 */
[[nodiscard]] inline grouped_soa_query_reader_memory_stats
prepared_minimizer_fragment_reader::memory_stats() const noexcept {
    const std::uint64_t directory_bytes = directory_.owned_capacity_bytes();
    const std::uint64_t entry_plan_bytes =
        bioseq::common::saturating_mul(entry_count_, sizeof(entry_plan));
    const std::uint64_t block_plan_bytes =
        bioseq::common::saturating_mul(block_count_, sizeof(fragment_block_plan));
    return grouped_soa_query_reader_memory_stats{
        .capacity_accounting_passes = 1u,
        .global_position_storage_preparations = 0u,
        .global_position_storage_allocations = 0u,
        .position_append_growth_reservations = 0u,
        .retained_position_count = 0u,
        .retained_position_capacity = 0u,
        .query_record_size_bytes = 0u,
        .query_record_count = 0u,
        .query_record_capacity = 0u,
        .query_record_capacity_bytes = 0u,
        .query_record_position_capacity_bytes = 0u,
        .query_hash_capacity_bytes = 0u,
        .released_query_hash_capacity_bytes = 0u,
        .query_route_radix_bits = 0u,
        .query_route_radix_bucket_count = 0u,
        .query_route_radix_nonempty_bucket_count = 0u,
        .query_route_radix_max_bucket_size = 0u,
        .query_route_control_lookups = 0u,
        .query_route_hash_probes = 0u,
        .query_route_matches = 0u,
        .decoded_control_records = 0u,
        .retained_selected_control_records = 0u,
        .selected_control_size_bytes = 0u,
        .retained_selected_control_capacity_bytes = 0u,
        .current_selected_control_capacity_bytes = 0u,
        .directory_capacity_bytes = directory_bytes,
        .ordinal_route_capacity_bytes =
            bioseq::common::saturating_add(entry_plan_bytes, block_plan_bytes),
        .selected_extent_capacity_bytes = 0u,
        .global_position_capacity_bytes = 0u,
        .control_payload_capacity_bytes = 0u,
        .owned_capacity_bytes = owned_bytes_,
        .saturated = owned_bytes_ == (std::numeric_limits<std::uint64_t>::max)(),
    };
}

} // namespace bioseq::indexing
