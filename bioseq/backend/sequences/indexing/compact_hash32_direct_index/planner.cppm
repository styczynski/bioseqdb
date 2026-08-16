/**
 * @file planner.cppm
 * @brief Plans direct hash32 images and bounded query work.
 */
export module bioseq.indexing.compact_hash32_direct_index:planner;

import std;
import :wire_format;
import :writer;

export namespace bioseq::indexing {

/**
 * @brief Estimates compact image bytes from aggregate accepted-run cardinalities.
 *
 * The estimate uses the guaranteed minimum number of inline P32 singletons, so it is an upper
 * bound when only distinct-hash and posting totals are known.
 *
 * @param[in] geometry Hash-domain width and inclusive maximum
 * @param[in] accepted_hash_count Number of nonempty accepted hash runs
 * @param[in] accepted_posting_count Total postings in accepted runs
 * @param[in] posting_width_bytes Persisted posting width, either 4 or 8 bytes
 * @return Section and image byte estimate, or a geometry, width, or arithmetic error
 */
[[nodiscard]] constexpr std::expected<compact_hash32_image_estimate, direct_inverted_index_error>
estimate_compact_hash32_image_bytes(compact_hash32_geometry geometry,
                                    std::uint64_t accepted_hash_count,
                                    std::uint64_t accepted_posting_count,
                                    std::uint8_t posting_width_bytes) noexcept {
    if (geometry.hash_domain_bits < 9u ||
        geometry.hash_domain_bits > k_compact_hash32_full_hash_bits ||
        (posting_width_bytes != 4u && posting_width_bytes != 8u) ||
        accepted_posting_count > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(direct_inverted_index_error::invalid_options);
    }
    const std::uint32_t expected_max =
        geometry.hash_domain_bits == 32u
            ? (std::numeric_limits<std::uint32_t>::max)()
            : static_cast<std::uint32_t>((std::uint64_t{1u} << geometry.hash_domain_bits) - 1u);
    if (geometry.max_hash != expected_max) {
        return std::unexpected(direct_inverted_index_error::storage_contract_mismatch);
    }
    const std::uint64_t domain_hashes = std::uint64_t{1u} << geometry.hash_domain_bits;
    if (accepted_hash_count > domain_hashes || accepted_hash_count > accepted_posting_count) {
        return std::unexpected(direct_inverted_index_error::invalid_options);
    }
    compact_hash32_image_estimate estimate{
        .geometry = geometry,
        .posting_width_bytes = posting_width_bytes,
        .accepted_hash_count = accepted_hash_count,
        .accepted_posting_count = accepted_posting_count,
        // With U nonempty runs and P postings, at least 2U-P runs are
        // singletons.  The compact P32 encoding can inline that guaranteed
        // population; the estimate is therefore a conservative upper bound
        // when the caller has only aggregate U/P stats.
        .inline_singleton_count =
            posting_width_bytes == 4u &&
                    accepted_hash_count > accepted_posting_count - accepted_hash_count
                ? accepted_hash_count - (accepted_posting_count - accepted_hash_count)
                : 0u,
        .membership_bytes = (std::uint64_t{1u} << geometry.hash_domain_bits) / 8u,
        .rank_bytes = ((std::uint64_t{1u} << geometry.hash_domain_bits) /
                       k_compact_hash32_direct_index_rank_group_bits) *
                      4u,
    };
    estimate.materialized_posting_count = accepted_posting_count - estimate.inline_singleton_count;
    estimate.materialized_hash_count = accepted_hash_count - estimate.inline_singleton_count;
    if (!direct_index_detail::checked_multiply(accepted_hash_count,
                                               k_compact_hash32_direct_index_descriptor_bytes,
                                               estimate.descriptor_bytes) ||
        !direct_index_detail::checked_multiply(
            estimate.materialized_posting_count, posting_width_bytes, estimate.posting_bytes)) {
        return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
    }
    std::uint64_t cursor = k_compact_hash32_direct_index_header_bytes;
    std::uint64_t offset = 0u;
    if (!direct_index_detail::align_up(cursor, k_direct_inverted_index_section_alignment, offset) ||
        !direct_index_detail::checked_add(offset, estimate.membership_bytes, cursor) ||
        !direct_index_detail::align_up(cursor, k_direct_inverted_index_section_alignment, offset) ||
        !direct_index_detail::checked_add(offset, estimate.rank_bytes, cursor) ||
        !direct_index_detail::align_up(cursor, k_direct_inverted_index_section_alignment, offset) ||
        !direct_index_detail::checked_add(offset, estimate.descriptor_bytes, cursor) ||
        !direct_index_detail::align_up(cursor, k_direct_inverted_index_section_alignment, offset) ||
        !direct_index_detail::checked_add(offset, estimate.posting_bytes, estimate.image_bytes)) {
        return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
    }
    return estimate;
}

/**
 * @brief Bounds compact metadata bytes touched by a monotonic query batch.
 * @param[in] query_hashes Number of hashes probed
 * @param[in] distinct_rank_groups Number of 256-hash rank groups entered
 * @param[in] accepted_query_hashes Number of probes that require descriptors
 * @return Maximum membership, rank, descriptor, and total metadata bytes, or an input error
 */
[[nodiscard]] constexpr std::expected<compact_hash32_query_touch_estimate,
                                      direct_inverted_index_error>
estimate_compact_hash32_query_touches(std::uint64_t query_hashes,
                                      std::uint64_t distinct_rank_groups,
                                      std::uint64_t accepted_query_hashes) noexcept {
    if (distinct_rank_groups > query_hashes || accepted_query_hashes > query_hashes ||
        distinct_rank_groups > (std::uint64_t{1u} << 24u)) {
        return std::unexpected(direct_inverted_index_error::invalid_options);
    }
    compact_hash32_query_touch_estimate estimate{
        .query_hashes = query_hashes,
        .distinct_rank_groups = distinct_rank_groups,
        .accepted_query_hashes = accepted_query_hashes,
    };
    // One hash32 lookup popcounts at most four 64-bit words.  Monotonic batches
    // reuse the 4-byte rank checkpoint for every query in the same group.
    if (!direct_index_detail::checked_multiply(
            query_hashes, 32u, estimate.maximum_membership_bytes) ||
        !direct_index_detail::checked_multiply(
            distinct_rank_groups, 4u, estimate.maximum_rank_bytes) ||
        !direct_index_detail::checked_multiply(accepted_query_hashes,
                                               k_compact_hash32_direct_index_descriptor_bytes,
                                               estimate.maximum_descriptor_bytes) ||
        !direct_index_detail::checked_add(estimate.maximum_membership_bytes,
                                          estimate.maximum_rank_bytes,
                                          estimate.maximum_metadata_bytes) ||
        !direct_index_detail::checked_add(estimate.maximum_metadata_bytes,
                                          estimate.maximum_descriptor_bytes,
                                          estimate.maximum_metadata_bytes)) {
        return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
    }
    return estimate;
}

} // namespace bioseq::indexing

export namespace bioseq::indexing {

/**
 * @brief Scans a replayable source and plans an exact compact hash32 image.
 * @param[in] source Strictly ordered source that can be replayed for emission
 * @param[in] options Identity, filtering, geometry, and image limits
 * @return Exact section plan and stream receipt, or a source, layout, or admission error
 */
[[nodiscard]] inline std::expected<compact_hash32_image_plan, direct_inverted_index_error>
plan_compact_hash32_direct_index(const compact_hash32_sorted_run_source& source,
                                 const compact_hash32_build_options& options) {
    if (options.max_occ_cap > (std::numeric_limits<std::uint16_t>::max)()) {
        return std::unexpected(direct_inverted_index_error::invalid_options);
    }
    const auto receipt = compact_hash32_detail::scan_source(source, options);
    if (!receipt)
        return std::unexpected(receipt.error());
    auto plan = compact_hash32_detail::layout_from_receipt(options, *receipt);
    if (!plan)
        return std::unexpected(plan.error());
    if (plan->image_bytes > options.max_image_bytes ||
        plan->image_bytes > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        return std::unexpected(direct_inverted_index_error::admission);
    }
    return plan;
}

/**
 * @brief Plans and builds an owning compact hash32 direct-index image.
 * @param[in] source Strictly ordered source replayed once for planning and once for emission
 * @param[in] options Identity, filtering, geometry, and image limits
 * @return Complete authenticated image bytes, or a planning, replay, or allocation error
 */
[[nodiscard]] inline std::expected<std::vector<std::byte>, direct_inverted_index_error>
build_compact_hash32_direct_index(const compact_hash32_sorted_run_source& source,
                                  const compact_hash32_build_options& options) {
    const auto plan = plan_compact_hash32_direct_index(source, options);
    if (!plan)
        return std::unexpected(plan.error());
    try {
        std::vector<std::byte> image(static_cast<std::size_t>(plan->image_bytes), std::byte{0});
        if (auto encoded = build_compact_hash32_direct_index_into(source, options, *plan, image);
            !encoded) {
            return std::unexpected(encoded.error());
        }
        return image;
    } catch (const std::bad_alloc&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    } catch (const std::length_error&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    }
}

/**
 * @brief Plans compact lookups for a nondecreasing sequence of query occurrences.
 *
 * Repeated hashes reuse membership and descriptor work. The plan owns all selections, spans, and
 * per-occurrence results while the underlying image remains borrowed by the caller.
 *
 * @param[in] view Validated compact image
 * @param[in] queries Query hashes in nondecreasing order with query-strand orientation
 * @param[in] runtime_max_occ Positive runtime frequency ceiling no greater than the build cap
 * @return Prepared query plan, or an order, contract, allocation, or image error
 */
[[nodiscard]] inline std::expected<compact_hash32_query_plan, direct_inverted_index_error>
query_compact_hash32_direct_index(const compact_hash32_direct_index_view& view,
                                  std::span<const compact_hash32_query_occurrence> queries,
                                  std::uint32_t runtime_max_occ) {
    using namespace compact_hash32_detail;
    if (runtime_max_occ == 0u || runtime_max_occ > view.header.max_occ_cap ||
        queries.size() > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        return std::unexpected(direct_inverted_index_error::storage_contract_mismatch);
    }
    try {
        compact_hash32_query_plan plan{
            .image_header_crc64 = view.header.header_crc64,
            .max_occ_cap = view.header.max_occ_cap,
            .runtime_max_occ = runtime_max_occ,
            .posting_width_bytes = view.header.posting_width_bytes,
        };
        plan.results.resize(queries.size());
        plan.stats.query_occurrence_count = queries.size();
        bool have_previous = false;
        std::uint32_t previous_hash = 0u;
        bool previous_present = false;
        std::uint64_t previous_dense = 0u;
        compact_hash32_decoded_descriptor previous_descriptor{};
        std::uint64_t cached_rank_group = (std::numeric_limits<std::uint64_t>::max)();
        std::uint64_t cached_rank_base = 0u;
        std::uint32_t previous_selection = k_direct_inverted_index_no_selection;
        for (std::size_t ordinal = 0u; ordinal < queries.size(); ++ordinal) {
            const std::uint32_t hash = queries[ordinal].hash;
            if (have_previous && hash < previous_hash) {
                return std::unexpected(direct_inverted_index_error::order);
            }
            bool present = previous_present;
            std::uint64_t dense = previous_dense;
            compact_hash32_decoded_descriptor descriptor = previous_descriptor;
            if (!have_previous || hash != previous_hash) {
                ++plan.stats.distinct_query_hash_count;
                const auto address = resolve_compact_hash32_address(hash, view.header.geometry);
                if (!address)
                    return std::unexpected(address.error());
                if (cached_rank_group != address->rank_checkpoint_ordinal) {
                    cached_rank_group = address->rank_checkpoint_ordinal;
                    cached_rank_base = read_le<std::uint32_t>(
                        view.image,
                        static_cast<std::size_t>(view.header.rank_offset + cached_rank_group * 4u));
                    ++plan.stats.rank_checkpoints_examined;
                }
                const std::uint64_t word_in_group = address->bit_within_rank_group / 64u;
                const unsigned bit_in_word = address->bit_within_rank_group % 64u;
                std::uint64_t before = 0u;
                std::uint64_t selected_word = 0u;
                for (std::uint64_t word = 0u; word <= word_in_group; ++word) {
                    const std::uint64_t value = read_le<std::uint64_t>(
                        view.image,
                        static_cast<std::size_t>(view.header.membership_offset +
                                                 cached_rank_group * 32u + word * 8u));
                    ++plan.stats.membership_words_examined;
                    if (word < word_in_group) {
                        before += std::popcount(value);
                    } else {
                        selected_word = value;
                        const std::uint64_t lower_mask =
                            bit_in_word == 0u ? 0u : (std::uint64_t{1u} << bit_in_word) - 1u;
                        before += std::popcount(value & lower_mask);
                    }
                }
                present = (selected_word & (std::uint64_t{1u} << bit_in_word)) != 0u;
                dense = cached_rank_base + before;
                descriptor = {};
                if (present) {
                    if (dense >= view.header.accepted_hash_count) {
                        return std::unexpected(direct_inverted_index_error::dictionary);
                    }
                    descriptor = decode_descriptor(view.image,
                                                   view.header.descriptors_offset +
                                                       dense * view.header.descriptor_width_bytes,
                                                   view.header.posting_width_bytes);
                    ++plan.stats.descriptors_examined;
                    if (descriptor.global_count > runtime_max_occ) {
                        ++plan.stats.runtime_rejected_hash_count;
                        if (!checked_add(plan.stats.runtime_rejected_posting_count,
                                         descriptor.global_count,
                                         plan.stats.runtime_rejected_posting_count)) {
                            return std::unexpected(
                                direct_inverted_index_error::arithmetic_overflow);
                        }
                    }
                }
                previous_present = present;
                previous_dense = dense;
                previous_descriptor = descriptor;
                previous_selection = k_direct_inverted_index_no_selection;
            }
            compact_hash32_query_result& result = plan.results[ordinal];
            if (present && descriptor.global_count <= runtime_max_occ) {
                result.accepted = true;
                result.dense_ordinal = dense;
                result.global_count = descriptor.global_count;
                result.forward_count = queries[ordinal].query_strand_one
                                           ? descriptor.global_count - descriptor.strand_zero_count
                                           : descriptor.strand_zero_count;
                result.reverse_count = descriptor.global_count - result.forward_count;
                if (previous_selection == k_direct_inverted_index_no_selection) {
                    if (plan.selections.size() >= k_direct_inverted_index_no_selection) {
                        return std::unexpected(direct_inverted_index_error::query_size);
                    }
                    const std::uint32_t selection =
                        static_cast<std::uint32_t>(plan.selections.size());
                    const std::uint64_t first_span = plan.spans.size();
                    const std::uint64_t posting_byte_offset =
                        descriptor.inline_singleton
                            ? view.header.descriptors_offset +
                                  dense * view.header.descriptor_width_bytes
                            : view.header.postings_offset +
                                  static_cast<std::uint64_t>(descriptor.posting_ordinal) *
                                      view.header.posting_width_bytes;
                    if (descriptor.strand_zero_count != 0u) {
                        plan.spans.push_back(direct_index_posting_span{
                            .byte_offset = posting_byte_offset,
                            .count = descriptor.strand_zero_count,
                            .selection_ordinal = selection,
                            .target_strand_one = false,
                        });
                    }
                    const std::uint64_t strand_one =
                        descriptor.global_count - descriptor.strand_zero_count;
                    if (strand_one != 0u) {
                        plan.spans.push_back(direct_index_posting_span{
                            .byte_offset =
                                posting_byte_offset +
                                static_cast<std::uint64_t>(descriptor.strand_zero_count) *
                                    view.header.posting_width_bytes,
                            .count = strand_one,
                            .selection_ordinal = selection,
                            .target_strand_one = true,
                        });
                    }
                    plan.selections.push_back(compact_hash32_selection{
                        .hash = hash,
                        .dense_ordinal = dense,
                        .global_count = descriptor.global_count,
                        .target_strand_zero_count = descriptor.strand_zero_count,
                        .first_span = first_span,
                        .span_count = plan.spans.size() - first_span,
                    });
                    previous_selection = selection;
                    ++plan.stats.unique_selected_hash_count;
                    if (descriptor.inline_singleton) {
                        ++plan.stats.selected_inline_singleton_count;
                    } else {
                        ++plan.stats.selected_materialized_hash_count;
                        if (!checked_add(plan.stats.selected_materialized_posting_count,
                                         descriptor.global_count,
                                         plan.stats.selected_materialized_posting_count)) {
                            return std::unexpected(
                                direct_inverted_index_error::arithmetic_overflow);
                        }
                    }
                    if (!checked_add(plan.stats.selected_posting_count,
                                     descriptor.global_count,
                                     plan.stats.selected_posting_count)) {
                        return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
                    }
                }
                result.selection_ordinal = previous_selection;
            }
            previous_hash = hash;
            have_previous = true;
        }
        return plan;
    } catch (const std::bad_alloc&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    } catch (const std::length_error&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    }
}

/**
 * @brief Plans accepted compact lookups from a projected strictly increasing hash sequence.
 *
 * The projection avoids a duplicate contiguous hash owner. Construction reserves U mappings once,
 * fills without growth, and compacts to exactly c accepted mappings. Peak logical capacity is U+c,
 * at most 2U, and the returned plan retains c eight-byte mappings.
 *
 * @tparam p_hash_at Nonthrowing projection returning one exact 32-bit hash by ordinal
 * @param[in] view Validated compact image
 * @param[in] strictly_increasing_unique_hash_count Number of hashes exposed by `hash_at`
 * @param[in] hash_at Projection valid for every ordinal below the supplied count
 * @param[in] runtime_max_occ Positive runtime frequency ceiling no greater than the build cap
 * @return Authenticated accepted-only plan, or an order, contract, admission, or image error
 */
template <class p_hash_at>
    requires std::is_nothrow_invocable_v<p_hash_at&, std::size_t> &&
             std::is_same_v<std::invoke_result_t<p_hash_at&, std::size_t>, std::uint32_t>
[[nodiscard]] inline std::expected<compact_hash32_unique_query_plan, direct_inverted_index_error>
query_compact_hash32_direct_index_unique_projected(
    const compact_hash32_direct_index_view& view,
    std::size_t strictly_increasing_unique_hash_count,
    p_hash_at hash_at,
    std::uint32_t runtime_max_occ) {
    using namespace compact_hash32_detail;
    if (runtime_max_occ == 0u || runtime_max_occ > view.header.max_occ_cap ||
        strictly_increasing_unique_hash_count >=
            static_cast<std::size_t>(k_direct_inverted_index_no_selection)) {
        return std::unexpected(direct_inverted_index_error::storage_contract_mismatch);
    }
    try {
        compact_hash32_unique_query_plan plan{};
        plan.image_header_crc64 = view.header.header_crc64;
        plan.max_occ_cap = view.header.max_occ_cap;
        plan.runtime_max_occ = runtime_max_occ;
        plan.posting_width_bytes = view.header.posting_width_bytes;
        plan.stats.query_occurrence_count = strictly_increasing_unique_hash_count;
        const std::size_t reserve_request = strictly_increasing_unique_hash_count;
        if (reserve_request > plan.selections.max_size()) {
            return std::unexpected(direct_inverted_index_error::query_size);
        }
        plan.stats.selection_reserve_call_count = 1u;
        plan.stats.selection_reserve_requested_elements = reserve_request;
        plan.selections.reserve(reserve_request);
        if (plan.selections.capacity() != reserve_request) {
            return std::unexpected(direct_inverted_index_error::admission);
        }
        plan.stats.selection_initial_capacity_elements = plan.selections.capacity();
        plan.stats.selection_storage_allocation_count = plan.selections.capacity() == 0u ? 0u : 1u;
        plan.stats.selection_peak_capacity_bytes = plan.retained_selection_capacity_bytes();
        bool have_previous = false;
        std::uint32_t previous_hash = 0u;
        std::uint64_t cached_rank_group = (std::numeric_limits<std::uint64_t>::max)();
        std::uint64_t cached_rank_base = 0u;
        std::array<std::uint64_t, 4u> cached_membership_words{};
        std::array<std::uint64_t, 5u> cached_membership_prefix_popcounts{};
        for (std::size_t query_ordinal = 0u; query_ordinal < strictly_increasing_unique_hash_count;
             ++query_ordinal) {
            const std::uint32_t hash = hash_at(query_ordinal);
            if (have_previous && hash <= previous_hash) {
                return std::unexpected(direct_inverted_index_error::order);
            }
            ++plan.stats.distinct_query_hash_count;
            const auto address = resolve_compact_hash32_address(hash, view.header.geometry);
            if (!address)
                return std::unexpected(address.error());
            if (cached_rank_group != address->rank_checkpoint_ordinal) {
                cached_rank_group = address->rank_checkpoint_ordinal;
                cached_rank_base = read_le<std::uint32_t>(
                    view.image,
                    static_cast<std::size_t>(view.header.rank_offset + cached_rank_group * 4u));
                ++plan.stats.rank_checkpoints_examined;
                cached_membership_prefix_popcounts[0u] = 0u;
                for (std::size_t word = 0u; word < cached_membership_words.size(); ++word) {
                    cached_membership_words[word] = read_le<std::uint64_t>(
                        view.image,
                        static_cast<std::size_t>(view.header.membership_offset +
                                                 cached_rank_group * 32u + word * 8u));
                    ++plan.stats.membership_words_examined;
                    cached_membership_prefix_popcounts[word + 1u] =
                        cached_membership_prefix_popcounts[word] +
                        std::popcount(cached_membership_words[word]);
                }
            }
            const std::size_t word_in_group =
                static_cast<std::size_t>(address->bit_within_rank_group / 64u);
            const unsigned bit_in_word = address->bit_within_rank_group % 64u;
            const std::uint64_t selected_word = cached_membership_words[word_in_group];
            const std::uint64_t lower_mask =
                bit_in_word == 0u ? 0u : (std::uint64_t{1u} << bit_in_word) - 1u;
            const std::uint64_t before = cached_membership_prefix_popcounts[word_in_group] +
                                         std::popcount(selected_word & lower_mask);
            const bool present = (selected_word & (std::uint64_t{1u} << bit_in_word)) != 0u;
            const std::uint64_t dense = cached_rank_base + before;
            if (!present) {
                previous_hash = hash;
                have_previous = true;
                continue;
            }
            if (dense >= view.header.accepted_hash_count) {
                return std::unexpected(direct_inverted_index_error::dictionary);
            }
            const compact_hash32_decoded_descriptor descriptor = decode_descriptor(
                view.image,
                view.header.descriptors_offset + dense * view.header.descriptor_width_bytes,
                view.header.posting_width_bytes);
            ++plan.stats.descriptors_examined;
            if (descriptor.global_count > runtime_max_occ) {
                ++plan.stats.runtime_rejected_hash_count;
                if (!checked_add(plan.stats.runtime_rejected_posting_count,
                                 descriptor.global_count,
                                 plan.stats.runtime_rejected_posting_count)) {
                    return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
                }
                previous_hash = hash;
                have_previous = true;
                continue;
            }
            if (plan.selections.size() >= k_direct_inverted_index_no_selection) {
                return std::unexpected(direct_inverted_index_error::query_size);
            }
            const compact_hash32_unique_selection* const storage_before_push =
                plan.selections.data();
            const std::size_t capacity_before_push = plan.selections.capacity();
            plan.selections.push_back(compact_hash32_unique_selection{
                .dense_ordinal = static_cast<std::uint32_t>(dense),
                .query_ordinal = static_cast<std::uint32_t>(query_ordinal),
            });
            if (plan.selections.data() != storage_before_push ||
                plan.selections.capacity() != capacity_before_push) {
                ++plan.stats.selection_fill_reallocation_count;
            }
            ++plan.stats.unique_selected_hash_count;
            if (descriptor.inline_singleton) {
                ++plan.stats.selected_inline_singleton_count;
            } else {
                ++plan.stats.selected_materialized_hash_count;
                if (!checked_add(plan.stats.selected_materialized_posting_count,
                                 descriptor.global_count,
                                 plan.stats.selected_materialized_posting_count)) {
                    return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
                }
            }
            if (!checked_add(plan.stats.selected_posting_count,
                             descriptor.global_count,
                             plan.stats.selected_posting_count)) {
                return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
            }
            previous_hash = hash;
            have_previous = true;
        }
        if (plan.stats.selection_fill_reallocation_count != 0u) {
            return std::unexpected(direct_inverted_index_error::admission);
        }
        const std::size_t accepted_count = plan.selections.size();
        if (accepted_count < reserve_request) {
            if (accepted_count == 0u) {
                std::vector<compact_hash32_unique_selection> empty;
                plan.selections.swap(empty);
            } else {
                std::vector<compact_hash32_unique_selection> compacted;
                if (accepted_count > compacted.max_size()) {
                    return std::unexpected(direct_inverted_index_error::query_size);
                }
                compacted.reserve(accepted_count);
                if (compacted.capacity() != accepted_count) {
                    return std::unexpected(direct_inverted_index_error::admission);
                }
                ++plan.stats.selection_compaction_allocation_count;
                ++plan.stats.selection_storage_allocation_count;
                const std::uint64_t compacted_capacity_bytes =
                    accepted_count > (std::numeric_limits<std::uint64_t>::max)() /
                                         sizeof(compact_hash32_unique_selection)
                        ? (std::numeric_limits<std::uint64_t>::max)()
                        : static_cast<std::uint64_t>(accepted_count) *
                              sizeof(compact_hash32_unique_selection);
                if (!checked_add(plan.stats.selection_peak_capacity_bytes,
                                 compacted_capacity_bytes,
                                 plan.stats.selection_peak_capacity_bytes)) {
                    return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
                }
                const compact_hash32_unique_selection* const compacted_storage = compacted.data();
                for (const compact_hash32_unique_selection selection : plan.selections) {
                    compacted.push_back(selection);
                }
                if (compacted.size() != accepted_count || compacted.capacity() != accepted_count ||
                    compacted.data() != compacted_storage) {
                    return std::unexpected(direct_inverted_index_error::admission);
                }
                plan.selections.swap(compacted);
            }
        }
        if (plan.selections.size() != accepted_count ||
            plan.selections.capacity() != accepted_count) {
            return std::unexpected(direct_inverted_index_error::admission);
        }
        plan.stats.selection_retained_capacity_elements = plan.selections.capacity();
        plan.stats.selection_retained_capacity_bytes = plan.retained_selection_capacity_bytes();
        compact_hash32_unique_query_plan_access::seal(plan);
        return plan;
    } catch (const std::bad_alloc&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    } catch (const std::length_error&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    }
}

/**
 * @brief Plans accepted compact lookups from a contiguous strictly increasing hash span.
 * @param[in] view Validated compact image
 * @param[in] strictly_increasing_unique_hashes Unique hashes in strict ascending order
 * @param[in] runtime_max_occ Positive runtime frequency ceiling no greater than the build cap
 * @return Authenticated accepted-only plan, or an order, contract, admission, or image error
 */
[[nodiscard]] inline std::expected<compact_hash32_unique_query_plan, direct_inverted_index_error>
query_compact_hash32_direct_index_unique(
    const compact_hash32_direct_index_view& view,
    std::span<const std::uint32_t> strictly_increasing_unique_hashes,
    std::uint32_t runtime_max_occ) {
    return query_compact_hash32_direct_index_unique_projected(
        view,
        strictly_increasing_unique_hashes.size(),
        [strictly_increasing_unique_hashes](std::size_t ordinal) noexcept {
            return strictly_increasing_unique_hashes[ordinal];
        },
        runtime_max_occ);
}

} // namespace bioseq::indexing

export namespace bioseq::indexing {

/**
 * @brief Plans a direct open-addressed image from unsorted input occurrences.
 * @param[in] occurrences Hash occurrences with 64-bit target coordinates
 * @param[in] options Identity, fragment-summary policy, and resource limits
 * @return Exact image and scratch plan, or a range, admission, allocation, or layout error
 */
[[nodiscard]] inline std::expected<direct_index_image_plan, direct_inverted_index_error>
plan_direct_inverted_index(std::span<const direct_index_occurrence> occurrences,
                           const direct_index_build_options& options) {
    auto prepared = direct_index_detail::prepare_build(occurrences, options);
    if (!prepared)
        return std::unexpected(prepared.error());
    return prepared->plan;
}

/**
 * @brief Plans direct-index posting spans for arbitrary query occurrence order.
 *
 * Each distinct accepted hash is selected once. Per-occurrence results retain query-strand-adjusted
 * forward and reverse counts, while fragment summaries split the posting range when present.
 *
 * @param[in] view Validated direct inverted-index image
 * @param[in] queries Query hashes and query-strand orientations
 * @param[in] max_occ Maximum occurrence count whose positions may be selected
 * @return Owning query plan, or a query-size, dictionary, allocation, or arithmetic error
 */
[[nodiscard]] inline std::expected<direct_index_query_plan, direct_inverted_index_error>
query_direct_inverted_index(const direct_inverted_index_view& view,
                            std::span<const direct_index_query_occurrence> queries,
                            std::uint64_t max_occ = (std::numeric_limits<std::uint64_t>::max)()) {
    using namespace direct_index_detail;
    if (queries.size() > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        return std::unexpected(direct_inverted_index_error::query_size);
    }
    try {
        direct_index_query_plan plan{
            .image_header_crc64 = view.header.header_crc64,
            .posting_width_bytes = view.header.posting_width_bytes,
        };
        plan.results.resize(queries.size());
        plan.stats.query_occurrence_count = queries.size();
        std::unordered_map<std::uint32_t, std::uint32_t> selected_by_hash;
        selected_by_hash.reserve(std::min<std::size_t>(
            queries.size(),
            static_cast<std::size_t>(std::min<std::uint64_t>(
                view.header.unique_hash_count, (std::numeric_limits<std::size_t>::max)()))));
        for (std::size_t query_ordinal = 0u; query_ordinal < queries.size(); ++query_ordinal) {
            const auto found = lookup_control(view, queries[query_ordinal].hash, &plan.stats);
            if (!found)
                return std::unexpected(found.error());
            if (!*found)
                continue;
            const direct_index_decoded_control& control = (*found)->control;
            direct_index_query_result& result = plan.results[query_ordinal];
            result.present = true;
            result.global_count = control.global_count;
            result.forward_count = queries[query_ordinal].query_strand_one
                                       ? control.global_count - control.strand_zero_count
                                       : control.strand_zero_count;
            result.reverse_count = control.global_count - result.forward_count;
            if (control.global_count > max_occ)
                continue;
            result.positions_selected = true;
            const auto existing = selected_by_hash.find(control.hash);
            if (existing != selected_by_hash.end()) {
                result.selection_ordinal = existing->second;
                continue;
            }
            if (plan.selections.size() >= k_direct_inverted_index_no_selection) {
                return std::unexpected(direct_inverted_index_error::query_size);
            }
            const std::uint32_t selection_ordinal =
                static_cast<std::uint32_t>(plan.selections.size());
            const std::uint64_t first_span = plan.spans.size();
            if ((control.flags & k_flag_run_summaries) != 0u) {
                std::uint64_t summary_offset = control.summary_byte_offset;
                for (std::uint32_t run = 0u; run < control.summary_count; ++run) {
                    const direct_index_decoded_summary summary =
                        decode_summary(view.image, summary_offset);
                    ++plan.stats.run_summaries_examined;
                    if (summary.strand_zero_count != 0u) {
                        plan.spans.push_back(direct_index_posting_span{
                            .byte_offset = summary.posting_byte_offset,
                            .count = summary.strand_zero_count,
                            .selection_ordinal = selection_ordinal,
                            .fragment_ordinal = summary.fragment_ordinal,
                            .target_strand_one = false,
                        });
                    }
                    const std::uint64_t strand_one_count =
                        summary.total_count - summary.strand_zero_count;
                    if (strand_one_count != 0u) {
                        plan.spans.push_back(direct_index_posting_span{
                            .byte_offset =
                                summary.posting_byte_offset +
                                summary.strand_zero_count * view.header.posting_width_bytes,
                            .count = strand_one_count,
                            .selection_ordinal = selection_ordinal,
                            .fragment_ordinal = summary.fragment_ordinal,
                            .target_strand_one = true,
                        });
                    }
                    summary_offset += k_direct_inverted_index_run_summary_bytes;
                }
            } else {
                if (control.strand_zero_count != 0u) {
                    plan.spans.push_back(direct_index_posting_span{
                        .byte_offset = control.posting_byte_offset,
                        .count = control.strand_zero_count,
                        .selection_ordinal = selection_ordinal,
                        .target_strand_one = false,
                    });
                }
                const std::uint64_t strand_one_count =
                    control.global_count - control.strand_zero_count;
                if (strand_one_count != 0u) {
                    plan.spans.push_back(direct_index_posting_span{
                        .byte_offset = control.posting_byte_offset +
                                       control.strand_zero_count * view.header.posting_width_bytes,
                        .count = strand_one_count,
                        .selection_ordinal = selection_ordinal,
                        .target_strand_one = true,
                    });
                }
            }
            plan.selections.push_back(direct_index_selection{
                .hash = control.hash,
                .global_count = control.global_count,
                .target_strand_zero_count = control.strand_zero_count,
                .first_span = first_span,
                .span_count = plan.spans.size() - first_span,
            });
            selected_by_hash.emplace(control.hash, selection_ordinal);
            result.selection_ordinal = selection_ordinal;
            ++plan.stats.unique_selected_hash_count;
            if (!checked_add(plan.stats.selected_posting_count,
                             control.global_count,
                             plan.stats.selected_posting_count)) {
                return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
            }
        }
        if (!checked_multiply(plan.stats.selected_posting_count,
                              view.header.posting_width_bytes,
                              plan.stats.selected_posting_bytes)) {
            return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
        }
        return plan;
    } catch (const std::bad_alloc&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    } catch (const std::length_error&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    }
}

} // namespace bioseq::indexing
