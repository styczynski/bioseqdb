/**
 * @file reader.cppm
 * @brief Read the target positions selected by a direct-index query plan
 */
export module bioseq.indexing.compact_hash32_direct_index:reader;

import std;
import :wire_format;

export namespace bioseq::indexing {

/// Read selected target positions in selection order and call visitor for each.
/// Check each selection against the image's membership and descriptor data
/// before reading, it. A later error can occur after earlier callbacks have run.
///
/// @param view Checked compact image, kept alive and unchanged during the call.
/// @param plan Query plan for  this image and occurrence limit.
/// @param context Value passed unchanged to visitor.
/// @param visitor Callback returning true to continue, or false to stop with an error.
/// @return Counts of visited spans and positions, or an error.
[[nodiscard]] inline std::expected<direct_index_posting_visit_stats, direct_inverted_index_error>
visit_compact_hash32_direct_index_postings(const compact_hash32_direct_index_view& view,
                                           const compact_hash32_query_plan& plan,
                                           void* context,
                                           direct_index_posting_visitor visitor) {
    using namespace compact_hash32_detail;
    if (visitor == nullptr || plan.image_header_crc64 != view.header.header_crc64 ||
        plan.max_occ_cap != view.header.max_occ_cap || plan.runtime_max_occ == 0u ||
        plan.runtime_max_occ > view.header.max_occ_cap ||
        plan.posting_width_bytes != view.header.posting_width_bytes ||
        plan.stats.rejected_posting_reads != 0u ||
        (view.header.posting_width_bytes != 4u && view.header.posting_width_bytes != 8u)) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    std::uint64_t postings_end = 0u;
    std::uint64_t descriptors_end = 0u;
    if (!checked_add(view.header.postings_offset, view.header.postings_bytes, postings_end) ||
        postings_end > view.header.image_bytes ||
        !checked_add(
            view.header.descriptors_offset, view.header.descriptors_bytes, descriptors_end) ||
        descriptors_end > view.header.image_bytes ||
        plan.stats.unique_selected_hash_count != plan.selections.size()) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    direct_index_posting_visit_stats stats{};
    std::uint64_t expected_postings = 0u;
    std::uint64_t expected_first_span = 0u;
    bool have_previous_hash = false;
    std::uint32_t previous_hash = 0u;
    for (std::size_t selection_ordinal = 0u; selection_ordinal < plan.selections.size();
         ++selection_ordinal) {
        const compact_hash32_selection& selection = plan.selections[selection_ordinal];
        const auto address = resolve_compact_hash32_address(selection.hash, view.header.geometry);
        if (!address || (have_previous_hash && selection.hash <= previous_hash) ||
            selection.first_span != expected_first_span ||
            selection.first_span > plan.spans.size() ||
            selection.span_count > plan.spans.size() - selection.first_span) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }

        std::uint64_t rank_delta = 0u;
        std::uint64_t rank_offset = 0u;
        std::uint64_t membership_group_delta = 0u;
        if (!checked_multiply(address->rank_checkpoint_ordinal, 4u, rank_delta) ||
            !checked_add(view.header.rank_offset, rank_delta, rank_offset) ||
            !checked_multiply(address->rank_checkpoint_ordinal, 32u, membership_group_delta)) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        const std::uint64_t rank_base =
            read_le<std::uint32_t>(view.image, static_cast<std::size_t>(rank_offset));
        const std::uint64_t word_in_group = address->bit_within_rank_group / 64u;
        const unsigned bit_in_word = address->bit_within_rank_group % 64u;
        std::uint64_t before = 0u;
        std::uint64_t selected_word = 0u;
        for (std::uint64_t word = 0u; word <= word_in_group; ++word) {
            std::uint64_t word_delta = 0u;
            std::uint64_t membership_offset = 0u;
            if (!checked_multiply(word, 8u, word_delta) ||
                !checked_add(
                    view.header.membership_offset, membership_group_delta, membership_offset) ||
                !checked_add(membership_offset, word_delta, membership_offset)) {
                return std::unexpected(direct_inverted_index_error::query_plan);
            }
            const std::uint64_t value =
                read_le<std::uint64_t>(view.image, static_cast<std::size_t>(membership_offset));
            if (word < word_in_group) {
                before += std::popcount(value);
            } else {
                selected_word = value;
                const std::uint64_t lower_mask =
                    bit_in_word == 0u ? 0u : (std::uint64_t{1u} << bit_in_word) - 1u;
                before += std::popcount(value & lower_mask);
            }
        }
        const std::uint64_t dense_ordinal = rank_base + before;
        if ((selected_word & (std::uint64_t{1u} << bit_in_word)) == 0u ||
            dense_ordinal != selection.dense_ordinal ||
            dense_ordinal >= view.header.accepted_hash_count) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        std::uint64_t descriptor_delta = 0u;
        std::uint64_t descriptor_offset = 0u;
        if (!checked_multiply(
                dense_ordinal, view.header.descriptor_width_bytes, descriptor_delta) ||
            !checked_add(view.header.descriptors_offset, descriptor_delta, descriptor_offset) ||
            descriptor_offset > descriptors_end ||
            view.header.descriptor_width_bytes > descriptors_end - descriptor_offset) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        const compact_hash32_decoded_descriptor descriptor =
            decode_descriptor(view.image, descriptor_offset, view.header.posting_width_bytes);
        if (descriptor.strand_zero_count > descriptor.global_count) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        const std::uint64_t strand_one_count =
            descriptor.global_count - descriptor.strand_zero_count;
        const std::uint64_t expected_span_count =
            static_cast<std::uint64_t>(descriptor.strand_zero_count != 0u) +
            static_cast<std::uint64_t>(strand_one_count != 0u);
        std::uint64_t posting_delta = 0u;
        std::uint64_t posting_offset = 0u;
        if (descriptor.global_count != selection.global_count ||
            descriptor.strand_zero_count != selection.target_strand_zero_count ||
            descriptor.global_count > plan.runtime_max_occ ||
            selection.span_count != expected_span_count ||
            (!descriptor.inline_singleton &&
             (!checked_multiply(
                  descriptor.posting_ordinal, view.header.posting_width_bytes, posting_delta) ||
              !checked_add(view.header.postings_offset, posting_delta, posting_offset)))) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        if (descriptor.inline_singleton) {
            posting_offset = descriptor_offset;
        }
        std::uint64_t selection_postings = 0u;
        for (std::uint64_t relative = 0u; relative < selection.span_count; ++relative) {
            const direct_index_posting_span& span =
                plan.spans[static_cast<std::size_t>(selection.first_span + relative)];
            const bool expects_strand_zero = descriptor.strand_zero_count != 0u && relative == 0u;
            const bool expected_strand_one = !expects_strand_zero;
            const std::uint64_t expected_count =
                expects_strand_zero ? descriptor.strand_zero_count : strand_one_count;
            std::uint64_t expected_span_delta = 0u;
            std::uint64_t expected_span_offset = posting_offset;
            if (expected_strand_one && !checked_multiply(descriptor.strand_zero_count,
                                                         view.header.posting_width_bytes,
                                                         expected_span_delta)) {
                return std::unexpected(direct_inverted_index_error::query_plan);
            }
            if (!checked_add(expected_span_offset, expected_span_delta, expected_span_offset)) {
                return std::unexpected(direct_inverted_index_error::query_plan);
            }
            std::uint64_t span_bytes = 0u;
            std::uint64_t span_end = 0u;
            if (span.selection_ordinal != selection_ordinal || span.count == 0u ||
                !checked_multiply(span.count, view.header.posting_width_bytes, span_bytes) ||
                !checked_add(span.byte_offset, span_bytes, span_end) ||
                (descriptor.inline_singleton
                     ? (span.byte_offset != descriptor_offset || span_bytes != 4u ||
                        span_end != descriptor_offset + 4u)
                     : (span.byte_offset < view.header.postings_offset ||
                        (span.byte_offset - view.header.postings_offset) %
                                view.header.posting_width_bytes !=
                            0u ||
                        span_end > postings_end)) ||
                span.byte_offset != expected_span_offset || span.count != expected_count ||
                span.target_strand_one != expected_strand_one ||
                span.fragment_ordinal != k_direct_inverted_index_no_fragment ||
                !checked_add(selection_postings, span.count, selection_postings)) {
                return std::unexpected(direct_inverted_index_error::query_plan);
            }
            for (std::uint64_t ordinal = 0u; ordinal < span.count; ++ordinal) {
                const std::uint64_t encoded =
                    decode_posting(view.image,
                                   span.byte_offset + ordinal * view.header.posting_width_bytes,
                                   view.header.posting_width_bytes);
                if (((encoded & 1u) != 0u) != span.target_strand_one ||
                    (encoded >> 1u) >= view.header.identity.target_length) {
                    return std::unexpected(direct_inverted_index_error::query_plan);
                }
                if (!visitor(context,
                             direct_index_posting{
                                 .selection_ordinal = static_cast<std::uint32_t>(selection_ordinal),
                                 .encoded_position = encoded,
                                 .target_strand_one = span.target_strand_one,
                             })) {
                    return std::unexpected(direct_inverted_index_error::visitor_rejected);
                }
                ++stats.postings_visited;
            }
            ++stats.spans_visited;
        }
        if (selection_postings != selection.global_count ||
            !checked_add(expected_postings, selection_postings, expected_postings)) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        expected_first_span += selection.span_count;
        previous_hash = selection.hash;
        have_previous_hash = true;
    }
    if (expected_first_span != plan.spans.size() ||
        expected_postings != plan.stats.selected_posting_count ||
        stats.postings_visited != expected_postings) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    return stats;
}

/// Check an accepted-hash plan against its image and prepare it for range visits.
/// Recompute the plan checksum once here. Later visits kheck the saved binding
/// without scanning every selection again. Keep the image and plan alive and
/// unchanged while using the returned accessor.
[[nodiscard]] inline std::expected<compact_hash32_selection_span_range_accessor,
                                   direct_inverted_index_error>
bind_compact_hash32_direct_index_selection_spans(const compact_hash32_direct_index_view& view,
                                                 const compact_hash32_unique_query_plan& plan) {
    using namespace compact_hash32_detail;
    if (view.image.size() < k_compact_hash32_direct_index_header_bytes ||
        view.header.image_bytes != view.image.size() ||
        read_le<std::uint64_t>(view.image, 0u) != k_compact_hash32_direct_index_magic ||
        read_le<std::uint32_t>(view.image, 8u) != view.header.layout_revision ||
        read_le<std::uint64_t>(view.image, 208u) != view.header.header_crc64 ||
        read_le<std::uint64_t>(view.image, 216u) != view.header.image_bytes ||
        header_crc(view.image.first(k_compact_hash32_direct_index_header_bytes),
                   k_compact_hash32_header_crc_offset,
                   k_direct_inverted_index_checksum_bytes) != view.header.header_crc64) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    constexpr std::uint16_t expected_descriptor_width =
        k_compact_hash32_direct_index_descriptor_bytes;
    const std::uint64_t persisted_inline_singletons =
        read_le<std::uint64_t>(view.image, k_compact_hash32_inline_singleton_count_offset);
    std::uint64_t expected_descriptor_bytes = 0u;
    std::uint64_t expected_posting_bytes = 0u;
    if (view.header.layout_revision != k_compact_hash32_direct_index_layout_revision ||
        view.header.descriptor_width_bytes != expected_descriptor_width ||
        read_le<std::uint16_t>(view.image, 22u) != view.header.descriptor_width_bytes ||
        std::to_integer<std::uint8_t>(view.image[20u]) != view.header.posting_width_bytes ||
        read_le<std::uint64_t>(view.image, 48u) != view.header.identity.target_length ||
        read_le<std::uint32_t>(view.image, 56u) != view.header.max_occ_cap ||
        read_le<std::uint64_t>(view.image, 80u) != view.header.accepted_hash_count ||
        read_le<std::uint64_t>(view.image, 88u) != view.header.accepted_posting_count ||
        read_le<std::uint64_t>(view.image, 144u) != view.header.descriptors_offset ||
        read_le<std::uint64_t>(view.image, 152u) != view.header.descriptors_bytes ||
        read_le<std::uint64_t>(view.image, 160u) != view.header.postings_offset ||
        read_le<std::uint64_t>(view.image, 168u) != view.header.postings_bytes ||
        persisted_inline_singletons != view.header.inline_singleton_count ||
        (view.header.posting_width_bytes != 4u && view.header.posting_width_bytes != 8u) ||
        view.header.identity.target_length > (std::uint64_t{1u} << 63u) ||
        view.header.posting_width_bytes !=
            (view.header.identity.target_length <= (std::uint64_t{1u} << 31u) ? 4u : 8u) ||
        view.header.inline_singleton_count > view.header.accepted_hash_count ||
        view.header.inline_singleton_count > view.header.accepted_posting_count ||
        view.header.accepted_posting_count > (std::numeric_limits<std::uint32_t>::max)() ||
        (view.header.posting_width_bytes == 8u && view.header.inline_singleton_count != 0u) ||
        view.header.materialized_hash_count !=
            view.header.accepted_hash_count - view.header.inline_singleton_count ||
        view.header.materialized_posting_count !=
            view.header.accepted_posting_count - view.header.inline_singleton_count ||
        !checked_multiply(view.header.accepted_hash_count,
                          view.header.descriptor_width_bytes,
                          expected_descriptor_bytes) ||
        expected_descriptor_bytes != view.header.descriptors_bytes ||
        !checked_multiply(view.header.materialized_posting_count,
                          view.header.posting_width_bytes,
                          expected_posting_bytes) ||
        expected_posting_bytes != view.header.postings_bytes) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    std::uint64_t initial_capacity_bytes = 0u;
    std::uint64_t expected_peak_capacity_bytes = 0u;
    if (!checked_multiply(plan.stats.selection_initial_capacity_elements,
                          sizeof(compact_hash32_unique_selection),
                          initial_capacity_bytes)) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    const bool compacted_to_nonempty = plan.stats.selection_retained_capacity_elements != 0u &&
                                       plan.stats.selection_retained_capacity_elements <
                                           plan.stats.selection_initial_capacity_elements;
    expected_peak_capacity_bytes = initial_capacity_bytes;
    if (compacted_to_nonempty && !checked_add(expected_peak_capacity_bytes,
                                              plan.stats.selection_retained_capacity_bytes,
                                              expected_peak_capacity_bytes)) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    const std::uint64_t expected_storage_allocations =
        (plan.stats.selection_initial_capacity_elements == 0u ? 0u : 1u) +
        (compacted_to_nonempty ? 1u : 0u);
    if (!compact_hash32_unique_query_plan_access::authenticated(plan) ||
        plan.image_header_crc64 != view.header.header_crc64 ||
        plan.max_occ_cap != view.header.max_occ_cap || plan.runtime_max_occ == 0u ||
        plan.runtime_max_occ > view.header.max_occ_cap ||
        plan.posting_width_bytes != view.header.posting_width_bytes ||
        (view.header.posting_width_bytes != 4u && view.header.posting_width_bytes != 8u) ||
        plan.stats.unique_selected_hash_count != plan.selections.size() ||
        plan.stats.rejected_posting_reads != 0u || plan.stats.selection_reserve_call_count != 1u ||
        plan.stats.selection_reserve_requested_elements != plan.stats.query_occurrence_count ||
        plan.stats.selection_initial_capacity_elements !=
            plan.stats.selection_reserve_requested_elements ||
        plan.stats.selection_retained_capacity_elements >
            plan.stats.selection_initial_capacity_elements ||
        plan.stats.selection_fill_reallocation_count != 0u ||
        plan.stats.selection_compaction_allocation_count != (compacted_to_nonempty ? 1u : 0u) ||
        plan.stats.selection_storage_allocation_count != expected_storage_allocations ||
        plan.stats.selection_peak_capacity_bytes != expected_peak_capacity_bytes ||
        plan.stats.selection_retained_capacity_elements != plan.selections.size() ||
        plan.stats.selection_retained_capacity_elements != plan.selections.capacity() ||
        plan.stats.selection_retained_capacity_bytes != plan.retained_selection_capacity_bytes() ||
        plan.selections.size() >= k_direct_inverted_index_no_selection) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    std::uint64_t postings_end = 0u;
    std::uint64_t descriptors_end = 0u;
    if (!checked_add(view.header.postings_offset, view.header.postings_bytes, postings_end) ||
        postings_end != view.header.image_bytes ||
        !checked_add(
            view.header.descriptors_offset, view.header.descriptors_bytes, descriptors_end) ||
        view.header.descriptors_offset < k_compact_hash32_direct_index_header_bytes ||
        descriptors_end > view.header.postings_offset ||
        view.header.postings_offset > postings_end) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }

    return compact_hash32_selection_span_range_accessor{
        view,
        plan,
        compact_hash32_unique_query_plan_access::authentication_receipt(plan),
        descriptors_end,
        postings_end,
    };
}

/// Visit selections in [selection_begin, selection_end), calling  visitor once
/// per hash with its nonempty strand spans. Check descriptor and span bounds
/// without decoding every position. Selection ordinals keep their original
/// plan indices, so callers can process adjacent ranges separately.
///
/// @tparam p_visitor Callback accepting compact_hash32_selection_posting_spans
/// and returning true to continue, or false to stop with an error.
template <class p_visitor>
[[nodiscard]] inline std::expected<compact_hash32_selection_span_visit_stats,
                                   direct_inverted_index_error>
visit_compact_hash32_direct_index_selection_span_range(
    const compact_hash32_selection_span_range_accessor& accessor,
    std::uint64_t selection_begin,
    std::uint64_t selection_end,
    p_visitor&& visitor) {
    using namespace compact_hash32_detail;
    if (accessor.plan_ == nullptr) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    const compact_hash32_unique_query_plan& plan = *accessor.plan_;
    if (compact_hash32_unique_query_plan_access::authentication_receipt(plan) !=
            accessor.plan_authentication_receipt_ ||
        plan.image_header_crc64 != accessor.plan_image_header_crc64_ ||
        plan.max_occ_cap != accessor.plan_max_occ_cap_ ||
        plan.runtime_max_occ != accessor.runtime_max_occ_ ||
        plan.posting_width_bytes != accessor.plan_posting_width_bytes_ ||
        plan.stats != accessor.plan_stats_ ||
        plan.selections.data() != accessor.selections_.data() ||
        plan.selections.size() != accessor.selections_.size() ||
        plan.selections.capacity() != accessor.selection_capacity_ ||
        selection_begin > selection_end || selection_end > accessor.selections_.size()) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }

    compact_hash32_selection_span_visit_stats stats{};
    std::uint64_t expected_postings = 0u;
    bool have_previous = selection_begin != 0u;
    std::uint32_t previous_dense_ordinal = 0u;
    std::uint32_t previous_query_ordinal = 0u;
    if (have_previous) {
        const compact_hash32_unique_selection& previous =
            accessor.selections_[static_cast<std::size_t>(selection_begin - 1u)];
        previous_dense_ordinal = previous.dense_ordinal;
        previous_query_ordinal = previous.query_ordinal;
    }
    for (std::uint64_t selection_ordinal = selection_begin; selection_ordinal < selection_end;
         ++selection_ordinal) {
        const compact_hash32_unique_selection& selection =
            accessor.selections_[static_cast<std::size_t>(selection_ordinal)];
        if (selection.query_ordinal >= accessor.plan_stats_.query_occurrence_count ||
            selection.dense_ordinal >= accessor.view_.header.accepted_hash_count ||
            (have_previous && (selection.dense_ordinal <= previous_dense_ordinal ||
                               selection.query_ordinal <= previous_query_ordinal))) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        std::uint64_t descriptor_delta = 0u;
        std::uint64_t descriptor_offset = 0u;
        if (!checked_multiply(selection.dense_ordinal,
                              accessor.view_.header.descriptor_width_bytes,
                              descriptor_delta) ||
            !checked_add(
                accessor.view_.header.descriptors_offset, descriptor_delta, descriptor_offset) ||
            descriptor_offset > accessor.descriptors_end_ ||
            accessor.view_.header.descriptor_width_bytes >
                accessor.descriptors_end_ - descriptor_offset) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        const compact_hash32_decoded_descriptor descriptor = decode_descriptor(
            accessor.view_.image, descriptor_offset, accessor.view_.header.posting_width_bytes);
        ++stats.descriptors_examined;
        if (descriptor.global_count == 0u || descriptor.global_count > accessor.runtime_max_occ_ ||
            descriptor.strand_zero_count > descriptor.global_count) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }

        compact_hash32_selection_posting_spans selected{
            .selection_ordinal = static_cast<std::uint32_t>(selection_ordinal),
            .dense_ordinal = selection.dense_ordinal,
            .query_ordinal = selection.query_ordinal,
            .global_count = descriptor.global_count,
            .strand_zero_count = descriptor.strand_zero_count,
        };
        for (std::uint32_t strand = 0u; strand < 2u; ++strand) {
            const bool strand_one = strand != 0u;
            const std::uint64_t count = strand_one
                                            ? descriptor.global_count - descriptor.strand_zero_count
                                            : descriptor.strand_zero_count;
            if (count == 0u)
                continue;
            const std::uint64_t ordinal_delta = strand_one ? descriptor.strand_zero_count : 0u;
            std::uint64_t posting_ordinal = 0u;
            std::uint64_t posting_delta = 0u;
            std::uint64_t posting_offset = 0u;
            std::uint64_t posting_bytes = 0u;
            std::uint64_t span_end = 0u;
            if (descriptor.inline_singleton) {
                posting_offset = descriptor_offset;
                posting_bytes = 4u;
                if (!checked_add(descriptor_offset, 4u, span_end) || count != 1u ||
                    descriptor.global_count != 1u ||
                    accessor.view_.header.posting_width_bytes != 4u ||
                    span_end > accessor.descriptors_end_) {
                    return std::unexpected(direct_inverted_index_error::query_plan);
                }
            } else if (!checked_add(descriptor.posting_ordinal, ordinal_delta, posting_ordinal) ||
                       !checked_multiply(posting_ordinal,
                                         accessor.view_.header.posting_width_bytes,
                                         posting_delta) ||
                       !checked_add(
                           accessor.view_.header.postings_offset, posting_delta, posting_offset) ||
                       !checked_multiply(
                           count, accessor.view_.header.posting_width_bytes, posting_bytes) ||
                       !checked_add(posting_offset, posting_bytes, span_end) ||
                       posting_offset < accessor.view_.header.postings_offset ||
                       (posting_offset - accessor.view_.header.postings_offset) %
                               accessor.view_.header.posting_width_bytes !=
                           0u ||
                       span_end > accessor.postings_end_) {
                return std::unexpected(direct_inverted_index_error::query_plan);
            }
            selected.spans[selected.span_count++] = compact_hash32_posting_span_view{
                .encoded_bytes =
                    accessor.view_.image.subspan(static_cast<std::size_t>(posting_offset),
                                                 static_cast<std::size_t>(posting_bytes)),
                .count = count,
                .target_length = accessor.view_.header.identity.target_length,
                .posting_width_bytes = accessor.view_.header.posting_width_bytes,
                .target_strand_one = strand_one,
            };
            ++stats.spans_exposed;
        }
        if (!checked_add(expected_postings, descriptor.global_count, expected_postings)) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        if (!static_cast<bool>(visitor(selected))) {
            return std::unexpected(direct_inverted_index_error::visitor_rejected);
        }
        ++stats.selection_callbacks;
        if (descriptor.inline_singleton) {
            ++stats.inline_selection_callbacks;
            ++stats.inline_postings_exposed;
        } else {
            ++stats.materialized_selection_callbacks;
            if (!checked_add(stats.materialized_postings_exposed,
                             descriptor.global_count,
                             stats.materialized_postings_exposed)) {
                return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
            }
        }
        if (!checked_add(stats.postings_exposed, descriptor.global_count, stats.postings_exposed)) {
            return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
        }
        previous_dense_ordinal = selection.dense_ordinal;
        previous_query_ordinal = selection.query_ordinal;
        have_previous = true;
    }
    const std::uint64_t range_selection_count = selection_end - selection_begin;
    const bool full_range = selection_begin == 0u && selection_end == accessor.selections_.size();
    if (stats.postings_exposed != expected_postings ||
        stats.selection_callbacks != range_selection_count ||
        stats.descriptors_examined != range_selection_count ||
        stats.inline_postings_exposed + stats.materialized_postings_exposed !=
            stats.postings_exposed ||
        stats.inline_selection_callbacks + stats.materialized_selection_callbacks !=
            stats.selection_callbacks ||
        stats.rank_checkpoints_examined != 0u || stats.membership_words_examined != 0u ||
        (full_range &&
         (expected_postings != accessor.plan_stats_.selected_posting_count ||
          stats.inline_selection_callbacks !=
              accessor.plan_stats_.selected_inline_singleton_count ||
          stats.materialized_selection_callbacks !=
              accessor.plan_stats_.selected_materialized_hash_count ||
          stats.inline_postings_exposed != accessor.plan_stats_.selected_inline_singleton_count ||
          stats.materialized_postings_exposed !=
              accessor.plan_stats_.selected_materialized_posting_count))) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    return stats;
}

/// Check the plan once, then visit all accepted hashes and their strand spans.
///
/// @tparam p_visitor Callback accepting compact_hash32_selection_posting_spans
/// and returning true to continue, or false to stop with an error
template <class p_visitor>
[[nodiscard]] inline std::expected<compact_hash32_selection_span_visit_stats,
                                   direct_inverted_index_error>
visit_compact_hash32_direct_index_selection_spans(const compact_hash32_direct_index_view& view,
                                                  const compact_hash32_unique_query_plan& plan,
                                                  p_visitor&& visitor) {
    const auto accessor = bind_compact_hash32_direct_index_selection_spans(view, plan);
    if (!accessor)
        return std::unexpected(accessor.error());
    return visit_compact_hash32_direct_index_selection_span_range(
        *accessor, 0u, accessor->selection_count(), std::forward<p_visitor>(visitor));
}

/// Decode the selected spans and call visitor once per target position.
/// Use the bulk span  visitor for plan and range checks, then validate each
/// position's strand and coordinate before passing it to the callback.
[[nodiscard]] inline std::expected<direct_index_posting_visit_stats, direct_inverted_index_error>
visit_compact_hash32_direct_index_postings(const compact_hash32_direct_index_view& view,
                                           const compact_hash32_unique_query_plan& plan,
                                           void* context,
                                           direct_index_posting_visitor visitor) {
    if (visitor == nullptr) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    direct_index_posting_visit_stats stats{};
    std::optional<direct_inverted_index_error> callback_error{};
    const auto visited = visit_compact_hash32_direct_index_selection_spans(
        view,
        plan,
        [&callback_error, &context, &visitor, &stats](
            const compact_hash32_selection_posting_spans& selected) noexcept {
            for (std::uint8_t span_ordinal = 0u; span_ordinal < selected.span_count;
                 ++span_ordinal) {
                const compact_hash32_posting_span_view& span = selected.spans[span_ordinal];
                for (std::uint64_t ordinal = 0u; ordinal < span.count; ++ordinal) {
                    const auto encoded = span.encoded_position_at(ordinal);
                    if (!encoded) {
                        callback_error = encoded.error();
                        return false;
                    }
                    if (!visitor(context,
                                 direct_index_posting{
                                     .selection_ordinal = selected.selection_ordinal,
                                     .encoded_position = *encoded,
                                     .target_strand_one = span.target_strand_one,
                                 })) {
                        callback_error = direct_inverted_index_error::visitor_rejected;
                        return false;
                    }
                    ++stats.postings_visited;
                }
                ++stats.spans_visited;
            }
            return true;
        });
    if (!visited) {
        return std::unexpected(callback_error.value_or(visited.error()));
    }
    if (stats.spans_visited != visited->spans_exposed ||
        stats.postings_visited != visited->postings_exposed) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    return stats;
}

} // namespace bioseq::indexing

export namespace bioseq::indexing {

/// Read positions selected from the direct index's hash table.
/// Check each position's strand, target bounds, and optional fragment assignmen
/// before calling visitor. Earlier callbacks remain visible if a later check fails.
///
/// @param view Checked image, kept alive, and unchanged during the call.
/// @param plan Query plan for the same image header and posting width.
/// @param context Value passed unchanged to visitor.
/// @param visitor Callback returning true to continue, or false to stop with an  error.
/// @return Counts of visited spans and positions, or an error.
[[nodiscard]] inline std::expected<direct_index_posting_visit_stats, direct_inverted_index_error>
visit_direct_inverted_index_postings(const direct_inverted_index_view& view,
                                     const direct_index_query_plan& plan,
                                     void* context,
                                     direct_index_posting_visitor visitor) {
    using namespace direct_index_detail;
    if (visitor == nullptr || plan.image_header_crc64 != view.header.header_crc64 ||
        plan.posting_width_bytes != view.header.posting_width_bytes) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    direct_index_posting_visit_stats stats{};
    std::uint64_t expected_postings = 0u;
    for (std::size_t selection_ordinal = 0u; selection_ordinal < plan.selections.size();
         ++selection_ordinal) {
        const direct_index_selection& selection = plan.selections[selection_ordinal];
        if (selection.first_span > plan.spans.size() ||
            selection.span_count > plan.spans.size() - selection.first_span) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
        std::uint64_t selection_postings = 0u;
        for (std::uint64_t relative = 0u; relative < selection.span_count; ++relative) {
            const direct_index_posting_span& span =
                plan.spans[static_cast<std::size_t>(selection.first_span + relative)];
            if (span.selection_ordinal != selection_ordinal || span.count == 0u ||
                span.byte_offset < view.header.postings_offset ||
                (span.byte_offset - view.header.postings_offset) %
                        view.header.posting_width_bytes !=
                    0u) {
                return std::unexpected(direct_inverted_index_error::query_plan);
            }
            std::uint64_t span_bytes = 0u;
            std::uint64_t span_end = 0u;
            if (!checked_multiply(span.count, view.header.posting_width_bytes, span_bytes) ||
                !checked_add(span.byte_offset, span_bytes, span_end) ||
                span_end > view.header.image_bytes ||
                !checked_add(selection_postings, span.count, selection_postings)) {
                return std::unexpected(direct_inverted_index_error::query_plan);
            }
            for (std::uint64_t ordinal = 0u; ordinal < span.count; ++ordinal) {
                const std::uint64_t encoded =
                    decode_position(view.image,
                                    span.byte_offset + ordinal * view.header.posting_width_bytes,
                                    view.header.posting_width_bytes);
                const bool strand_one = (encoded & 1u) != 0u;
                const std::uint64_t bp = encoded >> 1u;
                if (strand_one != span.target_strand_one ||
                    bp >= view.header.identity.target_length ||
                    (span.fragment_ordinal != k_direct_inverted_index_no_fragment &&
                     (view.header.fragment_span_bp == 0u ||
                      bp / view.header.fragment_span_bp != span.fragment_ordinal))) {
                    return std::unexpected(direct_inverted_index_error::query_plan);
                }
                if (!visitor(context,
                             direct_index_posting{
                                 .selection_ordinal = static_cast<std::uint32_t>(selection_ordinal),
                                 .fragment_ordinal = span.fragment_ordinal,
                                 .encoded_position = encoded,
                                 .target_strand_one = strand_one,
                             })) {
                    return std::unexpected(direct_inverted_index_error::visitor_rejected);
                }
                ++stats.postings_visited;
            }
            ++stats.spans_visited;
        }
        if (selection_postings != selection.global_count ||
            !checked_add(expected_postings, selection_postings, expected_postings)) {
            return std::unexpected(direct_inverted_index_error::query_plan);
        }
    }
    if (expected_postings != plan.stats.selected_posting_count ||
        stats.postings_visited != expected_postings) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    return stats;
}

} // namespace bioseq::indexing
