/**
 * @file open_validation.cppm
 * @brief Open direct-index images and check their stored layout and data.
 */
export module bioseq.indexing.compact_hash32_direct_index:open_validation;

import std;
import :wire_format;

export namespace bioseq::indexing {

/// Open a compact hash32 index by checking only its 256-byte header.
/// Verify  the identity, checksums of the header, and section bounds wxthout
/// reading the potentially large payload. Full payload checks belong to
/// validate_compact_hash32_direct_index().
///
/// @param mapped_owner Shared owner that keeps the mapped image alive.
/// @param direct_section Complete byte range of the compact index.
/// @param options Expected identity, hash domain, occurrence cap, and optional
/// checksum of the complete header.
/// @return A view that keeps mapped_owner alive, or a header-check error.
[[nodiscard]] inline std::expected<compact_hash32_bounded_open_view, direct_inverted_index_error>
open_compact_hash32_direct_index_bounded(std::shared_ptr<const void> mapped_owner,
                                         std::span<const std::byte> direct_section,
                                         const compact_hash32_bounded_open_options& options) {
    using namespace compact_hash32_detail;
    if (!mapped_owner) {
        return std::unexpected(direct_inverted_index_error::invalid_options);
    }
    if (direct_section.size() < k_compact_hash32_direct_index_header_bytes ||
        direct_section.data() == nullptr) {
        return std::unexpected(direct_inverted_index_error::image_size);
    }
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (direct_section.size() > (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(direct_inverted_index_error::image_size);
        }
    }

    // Restrict reads to the header. Payload checksums and padding are checked
    // by the full validator.
    const std::span<const std::byte> header_bytes =
        direct_section.first(k_compact_hash32_direct_index_header_bytes);
    if (read_le<std::uint64_t>(header_bytes, 0u) != k_compact_hash32_direct_index_magic) {
        return std::unexpected(direct_inverted_index_error::bad_magic);
    }
    const std::uint32_t layout_revision = read_le<std::uint32_t>(header_bytes, 8u);
    if (layout_revision != k_compact_hash32_direct_index_layout_revision) {
        return std::unexpected(direct_inverted_index_error::bad_layout_revision);
    }
    const std::uint16_t descriptor_width_bytes = read_le<std::uint16_t>(header_bytes, 22u);
    constexpr std::uint16_t expected_descriptor_width =
        k_compact_hash32_direct_index_descriptor_bytes;
    if (read_le<std::uint32_t>(header_bytes, 12u) != k_compact_hash32_direct_index_header_bytes ||
        read_le<std::uint32_t>(header_bytes, 16u) != 0u ||
        descriptor_width_bytes != expected_descriptor_width ||
        read_le<std::uint32_t>(header_bytes, 60u) !=
            k_compact_hash32_direct_index_rank_group_bits) {
        return std::unexpected(direct_inverted_index_error::bad_header);
    }

    compact_hash32_header header{};
    header.layout_revision = layout_revision;
    header.descriptor_width_bytes = descriptor_width_bytes;
    header.identity = direct_index_identity{
        .generation_id = read_le<std::uint64_t>(header_bytes, 24u),
        .source_identity = read_le<std::uint64_t>(header_bytes, 32u),
        .storage_contract_checksum = read_le<std::uint64_t>(header_bytes, 40u),
        .target_length = read_le<std::uint64_t>(header_bytes, 48u),
    };
    header.max_occ_cap = read_le<std::uint32_t>(header_bytes, 56u);
    header.posting_width_bytes = std::to_integer<std::uint8_t>(header_bytes[20u]);
    header.geometry = compact_hash32_geometry{
        .hash_domain_bits = std::to_integer<std::uint8_t>(header_bytes[21u]),
        .max_hash = read_le<std::uint32_t>(header_bytes, 232u),
    };
    header.source_hash_count = read_le<std::uint64_t>(header_bytes, 64u);
    header.source_posting_count = read_le<std::uint64_t>(header_bytes, 72u);
    header.accepted_hash_count = read_le<std::uint64_t>(header_bytes, 80u);
    header.accepted_posting_count = read_le<std::uint64_t>(header_bytes, 88u);
    header.inline_singleton_count =
        read_le<std::uint64_t>(header_bytes, k_compact_hash32_inline_singleton_count_offset);
    header.rejected_hash_count = read_le<std::uint64_t>(header_bytes, 96u);
    header.rejected_posting_count = read_le<std::uint64_t>(header_bytes, 104u);
    header.membership_offset = read_le<std::uint64_t>(header_bytes, 112u);
    header.membership_bytes = read_le<std::uint64_t>(header_bytes, 120u);
    header.rank_offset = read_le<std::uint64_t>(header_bytes, 128u);
    header.rank_bytes = read_le<std::uint64_t>(header_bytes, 136u);
    header.descriptors_offset = read_le<std::uint64_t>(header_bytes, 144u);
    header.descriptors_bytes = read_le<std::uint64_t>(header_bytes, 152u);
    header.postings_offset = read_le<std::uint64_t>(header_bytes, 160u);
    header.postings_bytes = read_le<std::uint64_t>(header_bytes, 168u);
    header.membership_crc64 = read_le<std::uint64_t>(header_bytes, 176u);
    header.rank_crc64 = read_le<std::uint64_t>(header_bytes, 184u);
    header.descriptors_crc64 = read_le<std::uint64_t>(header_bytes, 192u);
    header.postings_crc64 = read_le<std::uint64_t>(header_bytes, 200u);
    header.header_crc64 = read_le<std::uint64_t>(header_bytes, 208u);
    header.image_bytes = read_le<std::uint64_t>(header_bytes, 216u);
    header.sorted_stream_crc64 = read_le<std::uint64_t>(header_bytes, 224u);

    if (header_crc(header_bytes,
                   k_compact_hash32_header_crc_offset,
                   k_direct_inverted_index_checksum_bytes) != header.header_crc64) {
        return std::unexpected(direct_inverted_index_error::header_checksum);
    }
    if (options.expected_direct_header_crc64.has_value() &&
        *options.expected_direct_header_crc64 != crc64(header_bytes)) {
        return std::unexpected(direct_inverted_index_error::receipt_mismatch);
    }
    if (header.identity != options.expected_identity ||
        header.max_occ_cap != options.expected_max_occ_cap ||
        header.geometry != options.expected_geometry) {
        return std::unexpected(direct_inverted_index_error::identity_mismatch);
    }

    const auto geometry = geometry_counts(header.geometry);
    if (!geometry)
        return std::unexpected(geometry.error());
    const std::uint8_t expected_width =
        header.identity.target_length <= (std::uint64_t{1u} << 31u) ? 4u : 8u;
    const bool reserved_header_bytes_zero = direct_index_detail::all_zero(header_bytes.subspan(
        k_compact_hash32_inline_singleton_count_offset + sizeof(std::uint64_t), 12u));
    if (header.identity.target_length > (std::uint64_t{1u} << 63u) ||
        header.posting_width_bytes != expected_width ||
        header.max_occ_cap > (std::numeric_limits<std::uint16_t>::max)() ||
        !reserved_header_bytes_zero) {
        return std::unexpected(direct_inverted_index_error::bad_header);
    }

    const std::uint64_t direct_section_bytes = static_cast<std::uint64_t>(direct_section.size());
    if (header.image_bytes != direct_section_bytes ||
        header.inline_singleton_count > header.accepted_hash_count ||
        header.inline_singleton_count > header.accepted_posting_count ||
        (header.posting_width_bytes == 8u && header.inline_singleton_count != 0u) ||
        header.accepted_posting_count > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(direct_inverted_index_error::image_size);
    }
    std::uint64_t count_sum = 0u;
    std::uint64_t posting_sum = 0u;
    if (!checked_add(header.accepted_hash_count, header.rejected_hash_count, count_sum) ||
        count_sum != header.source_hash_count ||
        !checked_add(header.accepted_posting_count, header.rejected_posting_count, posting_sum) ||
        posting_sum != header.source_posting_count) {
        return std::unexpected(direct_inverted_index_error::control);
    }
    header.materialized_hash_count = header.accepted_hash_count - header.inline_singleton_count;
    header.materialized_posting_count =
        header.accepted_posting_count - header.inline_singleton_count;

    // Check section bounds before  calculating the expected layout. These are
    // offset calculations only, so the payload stays unread.
    const std::array sections{
        std::pair{header.membership_offset, header.membership_bytes},
        std::pair{header.rank_offset, header.rank_bytes},
        std::pair{header.descriptors_offset, header.descriptors_bytes},
        std::pair{header.postings_offset, header.postings_bytes},
    };
    std::uint64_t previous_end = k_compact_hash32_direct_index_header_bytes;
    for (const auto [offset, bytes] : sections) {
        std::uint64_t end = 0u;
        if (offset % k_direct_inverted_index_section_alignment != 0u || offset < previous_end ||
            !checked_add(offset, bytes, end) || end > header.image_bytes) {
            return std::unexpected(direct_inverted_index_error::section_layout);
        }
        previous_end = end;
    }
    if (previous_end != header.image_bytes) {
        return std::unexpected(direct_inverted_index_error::section_layout);
    }

    const compact_hash32_scan_receipt receipt{
        .source_hash_count = header.source_hash_count,
        .source_posting_count = header.source_posting_count,
        .accepted_hash_count = header.accepted_hash_count,
        .accepted_posting_count = header.accepted_posting_count,
        .inline_singleton_count = header.inline_singleton_count,
        .rejected_hash_count = header.rejected_hash_count,
        .rejected_posting_count = header.rejected_posting_count,
        .sorted_stream_crc64 = header.sorted_stream_crc64,
    };
    const compact_hash32_build_options layout_options{
        .identity = header.identity,
        .max_occ_cap = header.max_occ_cap,
        .geometry = header.geometry,
    };
    const auto layout = layout_from_receipt(layout_options, receipt);
    if (!layout || layout->layout_revision != header.layout_revision ||
        layout->descriptor_width_bytes != header.descriptor_width_bytes ||
        layout->inline_singleton_count != header.inline_singleton_count ||
        layout->materialized_hash_count != header.materialized_hash_count ||
        layout->materialized_posting_count != header.materialized_posting_count ||
        layout->membership_offset != header.membership_offset ||
        layout->membership_bytes != header.membership_bytes ||
        layout->rank_offset != header.rank_offset || layout->rank_bytes != header.rank_bytes ||
        layout->descriptors_offset != header.descriptors_offset ||
        layout->descriptors_bytes != header.descriptors_bytes ||
        layout->postings_offset != header.postings_offset ||
        layout->postings_bytes != header.postings_bytes ||
        layout->image_bytes != header.image_bytes) {
        return std::unexpected(direct_inverted_index_error::section_layout);
    }

    std::shared_ptr<const std::byte> image_owner{std::move(mapped_owner), direct_section.data()};
    return compact_hash32_bounded_open_view{
        std::move(image_owner),
        compact_hash32_direct_index_view{
            .image = direct_section,
            .header = header,
        },
        compact_hash32_bounded_open_stats{
            .direct_section_bytes = direct_section_bytes,
            .header_bytes_examined = k_compact_hash32_direct_index_header_bytes,
            .payload_bytes_examined = 0u,
            .expected_direct_header_crc64_checked =
                options.expected_direct_header_crc64.has_value(),
        },
    };
}

/// Check the complete compact hash32 index, including all stored positions.
/// Verify section sizes and CRCs, zero padding, membership counts, and each
/// hash's posting list Positions must stay within the target, and never decrease
/// within each strand. Equal positions are allowed.
///
/// @param image Complete image, kept alive and unchanged while the view is used.
/// @param expected_identity Required generation and source  identity.
/// @param expected_max_occ_cap Required occurrence limit used when building.
/// @param expected_geometry Required hash domain.
/// @return A view borrowing image, or the first validation error.
[[nodiscard]] inline std::expected<compact_hash32_direct_index_view, direct_inverted_index_error>
validate_compact_hash32_direct_index(std::span<const std::byte> image,
                                     const direct_index_identity& expected_identity,
                                     std::uint32_t expected_max_occ_cap,
                                     compact_hash32_geometry expected_geometry = {}) {
    using namespace compact_hash32_detail;
    if (image.size() < k_compact_hash32_direct_index_header_bytes) {
        return std::unexpected(direct_inverted_index_error::image_size);
    }
    if (read_le<std::uint64_t>(image, 0u) != k_compact_hash32_direct_index_magic) {
        return std::unexpected(direct_inverted_index_error::bad_magic);
    }
    const std::uint32_t layout_revision = read_le<std::uint32_t>(image, 8u);
    if (layout_revision != k_compact_hash32_direct_index_layout_revision) {
        return std::unexpected(direct_inverted_index_error::bad_layout_revision);
    }
    const std::uint16_t descriptor_width_bytes = read_le<std::uint16_t>(image, 22u);
    constexpr std::uint16_t expected_descriptor_width =
        k_compact_hash32_direct_index_descriptor_bytes;
    if (read_le<std::uint32_t>(image, 12u) != k_compact_hash32_direct_index_header_bytes ||
        read_le<std::uint32_t>(image, 16u) != 0u ||
        descriptor_width_bytes != expected_descriptor_width ||
        read_le<std::uint32_t>(image, 60u) != k_compact_hash32_direct_index_rank_group_bits) {
        return std::unexpected(direct_inverted_index_error::bad_header);
    }
    compact_hash32_header header{};
    header.layout_revision = layout_revision;
    header.descriptor_width_bytes = descriptor_width_bytes;
    header.identity = direct_index_identity{
        .generation_id = read_le<std::uint64_t>(image, 24u),
        .source_identity = read_le<std::uint64_t>(image, 32u),
        .storage_contract_checksum = read_le<std::uint64_t>(image, 40u),
        .target_length = read_le<std::uint64_t>(image, 48u),
    };
    header.max_occ_cap = read_le<std::uint32_t>(image, 56u);
    header.posting_width_bytes = std::to_integer<std::uint8_t>(image[20u]);
    header.geometry = compact_hash32_geometry{
        .hash_domain_bits = std::to_integer<std::uint8_t>(image[21u]),
        .max_hash = read_le<std::uint32_t>(image, 232u),
    };
    header.source_hash_count = read_le<std::uint64_t>(image, 64u);
    header.source_posting_count = read_le<std::uint64_t>(image, 72u);
    header.accepted_hash_count = read_le<std::uint64_t>(image, 80u);
    header.accepted_posting_count = read_le<std::uint64_t>(image, 88u);
    header.inline_singleton_count =
        read_le<std::uint64_t>(image, k_compact_hash32_inline_singleton_count_offset);
    header.rejected_hash_count = read_le<std::uint64_t>(image, 96u);
    header.rejected_posting_count = read_le<std::uint64_t>(image, 104u);
    header.membership_offset = read_le<std::uint64_t>(image, 112u);
    header.membership_bytes = read_le<std::uint64_t>(image, 120u);
    header.rank_offset = read_le<std::uint64_t>(image, 128u);
    header.rank_bytes = read_le<std::uint64_t>(image, 136u);
    header.descriptors_offset = read_le<std::uint64_t>(image, 144u);
    header.descriptors_bytes = read_le<std::uint64_t>(image, 152u);
    header.postings_offset = read_le<std::uint64_t>(image, 160u);
    header.postings_bytes = read_le<std::uint64_t>(image, 168u);
    header.membership_crc64 = read_le<std::uint64_t>(image, 176u);
    header.rank_crc64 = read_le<std::uint64_t>(image, 184u);
    header.descriptors_crc64 = read_le<std::uint64_t>(image, 192u);
    header.postings_crc64 = read_le<std::uint64_t>(image, 200u);
    header.header_crc64 = read_le<std::uint64_t>(image, 208u);
    header.image_bytes = read_le<std::uint64_t>(image, 216u);
    header.sorted_stream_crc64 = read_le<std::uint64_t>(image, 224u);

    if (header.identity != expected_identity || header.max_occ_cap != expected_max_occ_cap ||
        header.geometry.hash_domain_bits != expected_geometry.hash_domain_bits ||
        header.geometry.max_hash != expected_geometry.max_hash) {
        return std::unexpected(direct_inverted_index_error::identity_mismatch);
    }
    const auto geometry = geometry_counts(header.geometry);
    if (!geometry)
        return std::unexpected(geometry.error());
    const std::uint8_t expected_width =
        header.identity.target_length <= (std::uint64_t{1u} << 31u) ? 4u : 8u;
    const bool reserved_header_bytes_zero = direct_index_detail::all_zero(
        image.subspan(k_compact_hash32_inline_singleton_count_offset + sizeof(std::uint64_t), 12u));
    if (header.identity.target_length > (std::uint64_t{1u} << 63u) ||
        header.posting_width_bytes != expected_width ||
        header.max_occ_cap > (std::numeric_limits<std::uint16_t>::max)() ||
        !reserved_header_bytes_zero) {
        return std::unexpected(direct_inverted_index_error::bad_header);
    }
    if (header_crc(image.first(k_compact_hash32_direct_index_header_bytes),
                   k_compact_hash32_header_crc_offset,
                   k_direct_inverted_index_checksum_bytes) != header.header_crc64) {
        return std::unexpected(direct_inverted_index_error::header_checksum);
    }
    if (header.image_bytes != image.size() ||
        header.inline_singleton_count > header.accepted_hash_count ||
        header.inline_singleton_count > header.accepted_posting_count ||
        (header.posting_width_bytes == 8u && header.inline_singleton_count != 0u) ||
        header.accepted_posting_count > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(direct_inverted_index_error::image_size);
    }
    std::uint64_t count_sum = 0u;
    std::uint64_t posting_sum = 0u;
    if (!checked_add(header.accepted_hash_count, header.rejected_hash_count, count_sum) ||
        count_sum != header.source_hash_count ||
        !checked_add(header.accepted_posting_count, header.rejected_posting_count, posting_sum) ||
        posting_sum != header.source_posting_count) {
        return std::unexpected(direct_inverted_index_error::control);
    }
    header.materialized_hash_count = header.accepted_hash_count - header.inline_singleton_count;
    header.materialized_posting_count =
        header.accepted_posting_count - header.inline_singleton_count;
    const compact_hash32_scan_receipt receipt{
        .source_hash_count = header.source_hash_count,
        .source_posting_count = header.source_posting_count,
        .accepted_hash_count = header.accepted_hash_count,
        .accepted_posting_count = header.accepted_posting_count,
        .inline_singleton_count = header.inline_singleton_count,
        .rejected_hash_count = header.rejected_hash_count,
        .rejected_posting_count = header.rejected_posting_count,
        .sorted_stream_crc64 = header.sorted_stream_crc64,
    };
    const compact_hash32_build_options layout_options{
        .identity = header.identity,
        .max_occ_cap = header.max_occ_cap,
        .geometry = header.geometry,
    };
    const auto layout = layout_from_receipt(layout_options, receipt);
    if (!layout || layout->layout_revision != header.layout_revision ||
        layout->descriptor_width_bytes != header.descriptor_width_bytes ||
        layout->inline_singleton_count != header.inline_singleton_count ||
        layout->materialized_hash_count != header.materialized_hash_count ||
        layout->materialized_posting_count != header.materialized_posting_count ||
        layout->membership_offset != header.membership_offset ||
        layout->membership_bytes != header.membership_bytes ||
        layout->rank_offset != header.rank_offset || layout->rank_bytes != header.rank_bytes ||
        layout->descriptors_offset != header.descriptors_offset ||
        layout->descriptors_bytes != header.descriptors_bytes ||
        layout->postings_offset != header.postings_offset ||
        layout->postings_bytes != header.postings_bytes ||
        layout->image_bytes != header.image_bytes) {
        return std::unexpected(direct_inverted_index_error::section_layout);
    }
    // The checed layout makes these section slices safe to read.
    const auto section = [&image](std::uint64_t offset, std::uint64_t bytes) {
        return image.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(bytes));
    };
    if (!direct_index_detail::all_zero(
            image.subspan(k_compact_hash32_direct_index_header_bytes,
                          static_cast<std::size_t>(header.membership_offset -
                                                   k_compact_hash32_direct_index_header_bytes))) ||
        !direct_index_detail::all_zero(image.subspan(
            static_cast<std::size_t>(header.membership_offset + header.membership_bytes),
            static_cast<std::size_t>(header.rank_offset - header.membership_offset -
                                     header.membership_bytes))) ||
        !direct_index_detail::all_zero(
            image.subspan(static_cast<std::size_t>(header.rank_offset + header.rank_bytes),
                          static_cast<std::size_t>(header.descriptors_offset - header.rank_offset -
                                                   header.rank_bytes))) ||
        !direct_index_detail::all_zero(image.subspan(
            static_cast<std::size_t>(header.descriptors_offset + header.descriptors_bytes),
            static_cast<std::size_t>(header.postings_offset - header.descriptors_offset -
                                     header.descriptors_bytes)))) {
        return std::unexpected(direct_inverted_index_error::section_layout);
    }
    if (crc64(section(header.membership_offset, header.membership_bytes)) !=
            header.membership_crc64 ||
        crc64(section(header.rank_offset, header.rank_bytes)) != header.rank_crc64 ||
        crc64(section(header.descriptors_offset, header.descriptors_bytes)) !=
            header.descriptors_crc64 ||
        crc64(section(header.postings_offset, header.postings_bytes)) != header.postings_crc64) {
        return std::unexpected(direct_inverted_index_error::section_checksum);
    }

    std::uint64_t accepted_seen = 0u;
    std::uint64_t accepted_postings_seen = 0u;
    std::uint64_t inline_singletons_seen = 0u;
    std::uint64_t materialized_hashes_seen = 0u;
    std::uint64_t materialized_posting_cursor = 0u;
    for (std::uint64_t group = 0u; group < geometry->rank_checkpoint_count; ++group) {
        if (accepted_seen > (std::numeric_limits<std::uint32_t>::max)() ||
            read_le<std::uint32_t>(image,
                                   static_cast<std::size_t>(header.rank_offset + group * 4u)) !=
                accepted_seen) {
            return std::unexpected(direct_inverted_index_error::dictionary);
        }
        for (std::uint64_t word_ordinal = 0u; word_ordinal < 4u; ++word_ordinal) {
            std::uint64_t word =
                read_le<std::uint64_t>(image,
                                       static_cast<std::size_t>(header.membership_offset +
                                                                group * 32u + word_ordinal * 8u));
            while (word != 0u) {
                const unsigned bit = std::countr_zero(word);
                if (accepted_seen >= header.accepted_hash_count) {
                    return std::unexpected(direct_inverted_index_error::dictionary);
                }
                const std::uint64_t descriptor_offset =
                    header.descriptors_offset + accepted_seen * header.descriptor_width_bytes;
                const compact_hash32_decoded_descriptor descriptor =
                    decode_descriptor(image, descriptor_offset, header.posting_width_bytes);
                if (descriptor.global_count == 0u || descriptor.global_count > header.max_occ_cap ||
                    descriptor.strand_zero_count > descriptor.global_count ||
                    accepted_postings_seen > header.accepted_posting_count ||
                    descriptor.global_count >
                        header.accepted_posting_count - accepted_postings_seen) {
                    return std::unexpected(direct_inverted_index_error::control);
                }
                std::array<std::uint64_t, 2> previous_bp{};
                std::array<bool, 2> have_previous_bp{};
                // Reject wrong strands, positions outside the target, and decreasing positions within a strand.
                const auto validate_encoded =
                    [&header, &descriptor, &have_previous_bp, &previous_bp](
                        std::uint64_t encoded, std::uint64_t ordinal) noexcept {
                        const bool strand_one = (encoded & 1u) != 0u;
                        const bool expected_strand = ordinal >= descriptor.strand_zero_count;
                        const std::uint64_t bp = encoded >> 1u;
                        if (strand_one != expected_strand || bp >= header.identity.target_length ||
                            (have_previous_bp[strand_one] && bp < previous_bp[strand_one])) {
                            return false;
                        }
                        previous_bp[strand_one] = bp;
                        have_previous_bp[strand_one] = true;
                        return true;
                    };
                if (descriptor.inline_singleton) {
                    if (header.posting_width_bytes != 4u || descriptor.global_count != 1u ||
                        !validate_encoded(descriptor.inline_encoded_position, 0u)) {
                        return std::unexpected(direct_inverted_index_error::posting);
                    }
                    ++inline_singletons_seen;
                } else {
                    if (materialized_posting_cursor > header.materialized_posting_count ||
                        descriptor.posting_ordinal != materialized_posting_cursor ||
                        descriptor.global_count >
                            header.materialized_posting_count - materialized_posting_cursor) {
                        return std::unexpected(direct_inverted_index_error::control);
                    }
                    for (std::uint64_t ordinal = 0u; ordinal < descriptor.global_count; ++ordinal) {
                        const std::uint64_t encoded = decode_posting(
                            image,
                            header.postings_offset + (materialized_posting_cursor + ordinal) *
                                                         header.posting_width_bytes,
                            header.posting_width_bytes);
                        if (!validate_encoded(encoded, ordinal)) {
                            return std::unexpected(direct_inverted_index_error::posting);
                        }
                    }
                    materialized_posting_cursor += descriptor.global_count;
                    ++materialized_hashes_seen;
                }
                accepted_postings_seen += descriptor.global_count;
                ++accepted_seen;
                word &= word - 1u;
                static_cast<void>(bit); // The bit position determines the hash represented by this descriptor.
            }
        }
    }
    if (accepted_seen != header.accepted_hash_count ||
        accepted_postings_seen != header.accepted_posting_count ||
        inline_singletons_seen != header.inline_singleton_count ||
        materialized_hashes_seen != header.materialized_hash_count ||
        materialized_posting_cursor != header.materialized_posting_count) {
        return std::unexpected(direct_inverted_index_error::dictionary);
    }
    return compact_hash32_direct_index_view{.image = image, .header = header};
}

} // namespace bioseq::indexing

export namespace bioseq::indexing {

/// Check a complete direct index that stores hashes in an open-addressed table.
/// Verify section CRCs and table lookups, then check each posting list and any
/// fragment summaries. The  returned view borrows the supplied bytes.
///
/// @param image Complete image, kept alive and unchanged while the view is used.
/// @param expected_identity Required generation and source identity.
/// @return A checked view, or the first validation error.
[[nodiscard]] inline std::expected<direct_inverted_index_view, direct_inverted_index_error>
validate_direct_inverted_index(std::span<const std::byte> image,
                               const direct_index_identity& expected_identity) {
    // Read every section and validate its contents here. Call this full check
    // when publishing or checking an image, since it also scans all postings.
    using namespace direct_index_detail;
    if (image.size() < k_direct_inverted_index_header_bytes) {
        return std::unexpected(direct_inverted_index_error::image_size);
    }
    if (read_le<std::uint64_t>(image, 0u) != k_direct_inverted_index_magic) {
        return std::unexpected(direct_inverted_index_error::bad_magic);
    }
    if (read_le<std::uint32_t>(image, 8u) != k_direct_inverted_index_layout_revision) {
        return std::unexpected(direct_inverted_index_error::bad_layout_revision);
    }
    if (read_le<std::uint32_t>(image, 12u) != k_direct_inverted_index_header_bytes) {
        return std::unexpected(direct_inverted_index_error::bad_header);
    }
    direct_index_header header{
        .flags = read_le<std::uint32_t>(image, 16u),
        .posting_width_bytes = std::to_integer<std::uint8_t>(image[20u]),
        .identity =
            direct_index_identity{
                .generation_id = read_le<std::uint64_t>(image, 24u),
                .source_identity = read_le<std::uint64_t>(image, 32u),
                .storage_contract_checksum = read_le<std::uint64_t>(image, 40u),
                .target_length = read_le<std::uint64_t>(image, 48u),
            },
        .occurrence_count = read_le<std::uint64_t>(image, 56u),
        .unique_hash_count = read_le<std::uint64_t>(image, 64u),
        .bucket_count = read_le<std::uint64_t>(image, 72u),
        .occupancy_offset = read_le<std::uint64_t>(image, 80u),
        .occupancy_bytes = read_le<std::uint64_t>(image, 88u),
        .rank_offset = read_le<std::uint64_t>(image, 96u),
        .rank_bytes = read_le<std::uint64_t>(image, 104u),
        .controls_offset = read_le<std::uint64_t>(image, 112u),
        .controls_bytes = read_le<std::uint64_t>(image, 120u),
        .summaries_offset = read_le<std::uint64_t>(image, 128u),
        .summaries_bytes = read_le<std::uint64_t>(image, 136u),
        .postings_offset = read_le<std::uint64_t>(image, 144u),
        .postings_bytes = read_le<std::uint64_t>(image, 152u),
        .fragment_span_bp = read_le<std::uint64_t>(image, 160u),
        .run_summary_count = read_le<std::uint64_t>(image, 168u),
        .occupancy_crc64 = read_le<std::uint64_t>(image, 176u),
        .rank_crc64 = read_le<std::uint64_t>(image, 184u),
        .controls_crc64 = read_le<std::uint64_t>(image, 192u),
        .summaries_crc64 = read_le<std::uint64_t>(image, 200u),
        .postings_crc64 = read_le<std::uint64_t>(image, 208u),
        .header_crc64 = read_le<std::uint64_t>(image, 216u),
        .image_bytes = read_le<std::uint64_t>(image, 224u),
    };
    if (header.identity != expected_identity) {
        return std::unexpected(direct_inverted_index_error::identity_mismatch);
    }
    if ((header.flags & ~k_flag_run_summaries) != 0u ||
        ((header.flags & k_flag_run_summaries) != 0u) != (header.fragment_span_bp != 0u) ||
        ((header.flags & k_flag_run_summaries) == 0u && header.run_summary_count != 0u)) {
        return std::unexpected(direct_inverted_index_error::bad_flags);
    }
    const std::uint8_t expected_width =
        header.identity.target_length <= (std::uint64_t{1u} << 31u) ? 4u : 8u;
    if (header.identity.target_length > (std::uint64_t{1u} << 63u) ||
        header.posting_width_bytes != expected_width ||
        read_le<std::uint32_t>(image, 232u) != k_direct_inverted_index_rank_group_buckets ||
        read_le<std::uint32_t>(image, 236u) != k_direct_inverted_index_control_bytes ||
        read_le<std::uint32_t>(image, 240u) != k_direct_inverted_index_run_summary_bytes ||
        read_le<std::uint32_t>(image, 244u) != k_direct_inverted_index_hash_mix_id) {
        return std::unexpected(direct_inverted_index_error::bad_header);
    }
    if (!all_zero(image.subspan(21u, 3u)) || !all_zero(image.subspan(248u, 8u))) {
        return std::unexpected(direct_inverted_index_error::reserved_nonzero);
    }
    const std::uint64_t computed_header_crc =
        header_crc(image.first(k_direct_inverted_index_header_bytes),
                   k_direct_inverted_index_header_crc_offset,
                   k_direct_inverted_index_checksum_bytes);
    if (computed_header_crc != header.header_crc64) {
        return std::unexpected(direct_inverted_index_error::header_checksum);
    }
    if (header.image_bytes != image.size()) {
        return std::unexpected(direct_inverted_index_error::image_size);
    }
    if (header.unique_hash_count > header.bucket_count - header.bucket_count / 8u) {
        return std::unexpected(direct_inverted_index_error::bucket_count);
    }
    const auto expected_layout = layout_from_counts(header.occurrence_count,
                                                    header.unique_hash_count,
                                                    header.bucket_count,
                                                    header.run_summary_count,
                                                    header.posting_width_bytes);
    if (!expected_layout || expected_layout->occupancy_offset != header.occupancy_offset ||
        expected_layout->occupancy_bytes != header.occupancy_bytes ||
        expected_layout->rank_offset != header.rank_offset ||
        expected_layout->rank_bytes != header.rank_bytes ||
        expected_layout->controls_offset != header.controls_offset ||
        expected_layout->controls_bytes != header.controls_bytes ||
        expected_layout->summaries_offset != header.summaries_offset ||
        expected_layout->summaries_bytes != header.summaries_bytes ||
        expected_layout->postings_offset != header.postings_offset ||
        expected_layout->postings_bytes != header.postings_bytes ||
        expected_layout->image_bytes != header.image_bytes) {
        return std::unexpected(direct_inverted_index_error::section_layout);
    }
    // Read section slices only after their offsets match the expected layout.
    const auto exact = [&image](std::uint64_t offset, std::uint64_t count) {
        return image.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(count));
    };
    if (!all_zero(image.subspan(k_direct_inverted_index_header_bytes,
                                static_cast<std::size_t>(header.occupancy_offset -
                                                         k_direct_inverted_index_header_bytes))) ||
        !all_zero(image.subspan(
            static_cast<std::size_t>(header.occupancy_offset + header.occupancy_bytes),
            static_cast<std::size_t>(header.rank_offset - header.occupancy_offset -
                                     header.occupancy_bytes))) ||
        !all_zero(
            image.subspan(static_cast<std::size_t>(header.rank_offset + header.rank_bytes),
                          static_cast<std::size_t>(header.controls_offset - header.rank_offset -
                                                   header.rank_bytes))) ||
        !all_zero(image.subspan(
            static_cast<std::size_t>(header.controls_offset + header.controls_bytes),
            static_cast<std::size_t>(header.summaries_offset - header.controls_offset -
                                     header.controls_bytes))) ||
        !all_zero(image.subspan(
            static_cast<std::size_t>(header.summaries_offset + header.summaries_bytes),
            static_cast<std::size_t>(header.postings_offset - header.summaries_offset -
                                     header.summaries_bytes)))) {
        return std::unexpected(direct_inverted_index_error::section_layout);
    }
    if (crc64(exact(header.occupancy_offset, header.occupancy_bytes)) != header.occupancy_crc64 ||
        crc64(exact(header.rank_offset, header.rank_bytes)) != header.rank_crc64 ||
        crc64(exact(header.controls_offset, header.controls_bytes)) != header.controls_crc64 ||
        crc64(exact(header.summaries_offset, header.summaries_bytes)) != header.summaries_crc64 ||
        crc64(exact(header.postings_offset, header.postings_bytes)) != header.postings_crc64) {
        return std::unexpected(direct_inverted_index_error::section_checksum);
    }

    direct_inverted_index_view view{.image = image, .header = header};
    const auto occupancy = exact(header.occupancy_offset, header.occupancy_bytes);
    const auto ranks = exact(header.rank_offset, header.rank_bytes);
    std::uint64_t occupied_count = 0u;
    std::uint64_t posting_cursor = header.postings_offset;
    std::uint64_t summary_cursor = header.summaries_offset;
    std::uint64_t dense_ordinal = 0u;
    const std::uint64_t rank_group_count =
        ceil_div(header.bucket_count, k_direct_inverted_index_rank_group_buckets);
    for (std::uint64_t slot = 0u; slot < header.bucket_count; ++slot) {
        if (slot % k_direct_inverted_index_rank_group_buckets == 0u) {
            if (read_le<std::uint64_t>(
                    ranks,
                    static_cast<std::size_t>(slot / k_direct_inverted_index_rank_group_buckets *
                                             8u)) != occupied_count) {
                return std::unexpected(direct_inverted_index_error::dictionary);
            }
        }
        if (!occupied(occupancy, slot))
            continue;
        ++occupied_count;
        const std::uint64_t control_offset =
            header.controls_offset + dense_ordinal * k_direct_inverted_index_control_bytes;
        const direct_index_decoded_control control = decode_control(image, control_offset);
        if (control.reserved_zero != 0u || control.global_count == 0u ||
            control.strand_zero_count > control.global_count ||
            control.posting_byte_offset != posting_cursor ||
            control.flags !=
                ((header.flags & k_flag_run_summaries) != 0u ? k_flag_run_summaries : 0u)) {
            return std::unexpected(direct_inverted_index_error::control);
        }
        const auto found = lookup_control(view, control.hash);
        if (!found || !*found || (*found)->slot != slot) {
            return std::unexpected(direct_inverted_index_error::dictionary);
        }
        std::uint64_t posting_bytes = 0u;
        if (!checked_multiply(control.global_count, header.posting_width_bytes, posting_bytes) ||
            !checked_add(posting_cursor, posting_bytes, posting_cursor) ||
            posting_cursor > header.image_bytes) {
            return std::unexpected(direct_inverted_index_error::posting);
        }
        if ((header.flags & k_flag_run_summaries) != 0u) {
            if (control.summary_count == 0u || control.summary_byte_offset != summary_cursor) {
                return std::unexpected(direct_inverted_index_error::run_summary);
            }
            std::uint64_t run_total = 0u;
            std::uint64_t run_strand_zero = 0u;
            std::uint64_t run_posting_cursor = control.posting_byte_offset;
            std::uint32_t previous_fragment = 0u;
            bool have_previous_fragment = false;
            std::uint64_t control_summary_bytes = 0u;
            std::uint64_t control_summary_end = 0u;
            if (!checked_multiply(control.summary_count,
                                  k_direct_inverted_index_run_summary_bytes,
                                  control_summary_bytes) ||
                !checked_add(summary_cursor, control_summary_bytes, control_summary_end) ||
                control_summary_end > header.summaries_offset + header.summaries_bytes) {
                return std::unexpected(direct_inverted_index_error::run_summary);
            }
            for (std::uint32_t run = 0u; run < control.summary_count; ++run) {
                const direct_index_decoded_summary summary = decode_summary(image, summary_cursor);
                if (summary.reserved_zero != 0u || summary.total_count == 0u ||
                    summary.strand_zero_count > summary.total_count ||
                    summary.posting_byte_offset != run_posting_cursor ||
                    (have_previous_fragment && summary.fragment_ordinal <= previous_fragment)) {
                    return std::unexpected(direct_inverted_index_error::run_summary);
                }
                std::uint64_t run_bytes = 0u;
                std::uint64_t run_end = 0u;
                if (summary.total_count > control.global_count - run_total ||
                    !checked_multiply(summary.total_count, header.posting_width_bytes, run_bytes) ||
                    !checked_add(run_posting_cursor, run_bytes, run_end) ||
                    run_end > posting_cursor) {
                    return std::unexpected(direct_inverted_index_error::run_summary);
                }
                std::array<std::uint64_t, 2> previous_bp{};
                std::array<bool, 2> have_previous_bp{};
                for (std::uint64_t ordinal = 0u; ordinal < summary.total_count; ++ordinal) {
                    const std::uint64_t position_offset =
                        run_posting_cursor + ordinal * header.posting_width_bytes;
                    const std::uint64_t encoded =
                        decode_position(image, position_offset, header.posting_width_bytes);
                    const bool strand_one = (encoded & 1u) != 0u;
                    const std::uint64_t bp = encoded >> 1u;
                    const bool expected_strand = ordinal >= summary.strand_zero_count;
                    if (strand_one != expected_strand || bp >= header.identity.target_length ||
                        bp / header.fragment_span_bp != summary.fragment_ordinal ||
                        (have_previous_bp[strand_one] && bp < previous_bp[strand_one])) {
                        return std::unexpected(direct_inverted_index_error::posting);
                    }
                    previous_bp[strand_one] = bp;
                    have_previous_bp[strand_one] = true;
                }
                run_posting_cursor = run_end;
                if (!checked_add(run_total, summary.total_count, run_total) ||
                    !checked_add(run_strand_zero, summary.strand_zero_count, run_strand_zero)) {
                    return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
                }
                previous_fragment = summary.fragment_ordinal;
                have_previous_fragment = true;
                summary_cursor += k_direct_inverted_index_run_summary_bytes;
            }
            if (run_total != control.global_count || run_strand_zero != control.strand_zero_count ||
                run_posting_cursor != posting_cursor) {
                return std::unexpected(direct_inverted_index_error::run_summary);
            }
        } else {
            if (control.summary_byte_offset != 0u || control.summary_count != 0u) {
                return std::unexpected(direct_inverted_index_error::control);
            }
            std::array<std::uint64_t, 2> previous_bp{};
            std::array<bool, 2> have_previous_bp{};
            for (std::uint64_t ordinal = 0u; ordinal < control.global_count; ++ordinal) {
                const std::uint64_t encoded = decode_position(
                    image,
                    control.posting_byte_offset + ordinal * header.posting_width_bytes,
                    header.posting_width_bytes);
                const bool strand_one = (encoded & 1u) != 0u;
                const std::uint64_t bp = encoded >> 1u;
                if (strand_one != (ordinal >= control.strand_zero_count) ||
                    bp >= header.identity.target_length ||
                    (have_previous_bp[strand_one] && bp < previous_bp[strand_one])) {
                    return std::unexpected(direct_inverted_index_error::posting);
                }
                previous_bp[strand_one] = bp;
                have_previous_bp[strand_one] = true;
            }
        }
        ++dense_ordinal;
    }
    if (read_le<std::uint64_t>(ranks, static_cast<std::size_t>(rank_group_count * 8u)) !=
            occupied_count ||
        occupied_count != header.unique_hash_count || dense_ordinal != header.unique_hash_count ||
        posting_cursor != header.image_bytes ||
        summary_cursor != header.summaries_offset + header.summaries_bytes) {
        return std::unexpected(direct_inverted_index_error::dictionary);
    }
    return view;
}

} // namespace bioseq::indexing
