/**
 * @file compact_direct/reader/codec.cppm
 * @brief Compact-direct control, position, matcher, and grouped leaf codecs.
 */

module;

#include <libassert/assert.hpp>

export module bioseq.indexing.minimizer_hashmap:compact_direct.reader_codec;

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

template <bool p_oracle, class p_visitor>
[[nodiscard]] inline std::optional<minimizer_generation_encoding_error>
prepared_minimizer_fragment_reader::with_query_group_occurrences_(
    const fragment_probe_context& context,
    const query_occurrence_group& group,
    p_visitor&& visitor) noexcept {
    const std::size_t begin_size = group.occurrence_ref_begin;
    const std::size_t count_size = static_cast<std::size_t>(group.occurrence_ref_count);
    const query_occurrence_packing_layout* occurrence_layout = nullptr;
    const packed_query_occurrence* packed_occurrences = nullptr;
    const wide_query_occurrence* wide_occurrences = nullptr;
    std::size_t packed_occurrence_count = 0u;
    std::size_t wide_occurrence_count = 0u;
    if constexpr (p_oracle) {
        DEBUG_ASSERT(context.scratch != nullptr, "oracle occurrence view is prevalidated");
        occurrence_layout = &context.scratch->occurrence_layout_;
        packed_occurrences = context.scratch->packed_occurrences_.data();
        wide_occurrences = context.scratch->wide_occurrences_.data();
        packed_occurrence_count = context.scratch->packed_occurrences_.size();
        wide_occurrence_count = context.scratch->wide_occurrences_.size();
    } else {
        DEBUG_ASSERT(context.bucket_direct_matcher != nullptr,
                     "production occurrence view is prevalidated");
        const bucket_direct_matcher_view& view = *context.bucket_direct_matcher;
        occurrence_layout = &view.occurrence_layout;
        packed_occurrences = view.packed_occurrences;
        wide_occurrences = view.wide_occurrences;
        packed_occurrence_count = view.packed_occurrence_count;
        wide_occurrence_count = view.wide_occurrence_count;
    }
    switch (occurrence_layout->route) {
    case query_occurrence_route::packed:
        DEBUG_ASSERT(begin_size <= packed_occurrence_count &&
                         count_size <= packed_occurrence_count - begin_size,
                     "packed group range is prevalidated");
        return std::forward<p_visitor>(visitor)(
            std::span<const packed_query_occurrence>{packed_occurrences + begin_size, count_size},
            *occurrence_layout);
    case query_occurrence_route::wide:
        DEBUG_ASSERT(begin_size <= wide_occurrence_count &&
                         count_size <= wide_occurrence_count - begin_size,
                     "wide group range is prevalidated");
        return std::forward<p_visitor>(visitor)(
            std::span<const wide_query_occurrence>{wide_occurrences + begin_size, count_size},
            *occurrence_layout);
    }
    return minimizer_generation_encoding_error::partition_id;
}

/// Adds forward and reverse anchor counts for one query occurrence using target strand
/// posting tallies.
[[nodiscard]] inline std::optional<minimizer_generation_encoding_error>
prepared_minimizer_fragment_reader::accumulate_orientation_(
    query_occurrence_reference occurrence, const orientation_occurrence_context& context) noexcept {
    if (context.scratch == nullptr || occurrence.batch_query_ordinal >= context.queries.size() ||
        occurrence.query_ordinal >=
            context.queries[occurrence.batch_query_ordinal].minimizers.size()) {
        return minimizer_generation_encoding_error::partition_id;
    }
    const std::size_t layout_ordinal = context.layout_base + occurrence.batch_query_ordinal;
    if (layout_ordinal < context.layout_base ||
        layout_ordinal >= context.scratch->block_query_layouts_.size()) {
        return minimizer_generation_encoding_error::arithmetic_overflow;
    }
    block_query_anchor_layout& layout = context.scratch->block_query_layouts_[layout_ordinal];
    const std::uint32_t query_strand =
        static_cast<std::uint32_t>(context.queries[occurrence.batch_query_ordinal]
                                       .minimizers[occurrence.query_ordinal]
                                       .pos_strand_encoded() &
                                   1u);
    const std::uint64_t forward =
        query_strand == 0u ? context.target_strand_zero : context.target_strand_one;
    const std::uint64_t reverse =
        query_strand == 0u ? context.target_strand_one : context.target_strand_zero;
    if (!add_checked_(layout.forward_count, forward) ||
        !add_checked_(layout.reverse_count, reverse)) {
        return minimizer_generation_encoding_error::arithmetic_overflow;
    }
    return std::nullopt;
}

/// Builds one fragment anchor for a block-bounded reference interval and stores it in
/// the shard query layout slice.
[[nodiscard]] inline std::optional<minimizer_generation_encoding_error>
prepared_minimizer_fragment_reader::emit_fragment_anchor_(
    std::uint64_t packed_target_position,
    query_occurrence_reference occurrence,
    const direct_emission_occurrence_context& context) noexcept {
    if (context.block == nullptr || context.scratch == nullptr || context.batch == nullptr ||
        context.queries.empty() || occurrence.batch_query_ordinal >= context.queries.size() ||
        occurrence.query_ordinal >=
            context.queries[occurrence.batch_query_ordinal].minimizers.size()) {
        return minimizer_generation_encoding_error::partition_id;
    }
    const std::size_t query_ordinal = occurrence.batch_query_ordinal;
    const auto anchor =
        make_fragment_anchor_(packed_target_position,
                              context.queries[query_ordinal].minimizers[occurrence.query_ordinal],
                              context.queries[query_ordinal].query_length,
                              static_cast<std::uint8_t>(context.queries[query_ordinal].kmer_size));
    if (!anchor || anchor->ref_position < context.block->reference_begin ||
        anchor->ref_position >= context.block->reference_end) {
        return anchor ? minimizer_generation_encoding_error::position_range : anchor.error();
    }
    if (context.block_ordinal >
        (std::numeric_limits<std::size_t>::max)() / context.queries.size()) {
        return minimizer_generation_encoding_error::arithmetic_overflow;
    }
    const std::size_t layout_base = context.block_ordinal * context.queries.size();
    const std::size_t layout_ordinal = layout_base + query_ordinal;
    if (layout_ordinal < layout_base ||
        layout_ordinal >= context.scratch->block_query_layouts_.size()) {
        return minimizer_generation_encoding_error::arithmetic_overflow;
    }
    block_query_anchor_layout& layout = context.scratch->block_query_layouts_[layout_ordinal];
    std::uint64_t* cursor =
        anchor->is_reverse != 0u ? &layout.reverse_cursor : &layout.forward_cursor;
    const std::uint64_t slice_begin =
        anchor->is_reverse != 0u ? layout.reverse_begin : layout.forward_begin;
    const std::uint64_t slice_count =
        anchor->is_reverse != 0u ? layout.reverse_count : layout.forward_count;
    if (slice_begin > (std::numeric_limits<std::uint64_t>::max)() - slice_count ||
        *cursor < slice_begin || *cursor >= slice_begin + slice_count ||
        *cursor >= context.batch->queries_[query_ordinal].anchor_count ||
        context.batch->queries_[query_ordinal].anchors == nullptr) {
        return minimizer_generation_encoding_error::arithmetic_overflow;
    }
    context.batch->queries_[query_ordinal].anchors[static_cast<std::size_t>(*cursor)] = *anchor;
    ++*cursor;
    return std::nullopt;
}

[[nodiscard]] inline bool prepared_minimizer_fragment_reader::reserve_query_occurrences_(
    occurrence_count_atomic& global_count,
    std::uint64_t local_count,
    std::uint32_t max_occ) noexcept {
    const std::uint32_t rejected = max_occ + 1u;
    std::uint32_t current = global_count.load(std::memory_order_relaxed);
    for (;;) {
        if (current > max_occ)
            return false;
        if (local_count == 0u || local_count > max_occ - current) {
            if (global_count.compare_exchange_weak(
                    current, rejected, std::memory_order_relaxed, std::memory_order_relaxed)) {
                return false;
            }
            continue;
        }
        if (global_count.compare_exchange_weak(current,
                                               current + static_cast<std::uint32_t>(local_count),
                                               std::memory_order_relaxed,
                                               std::memory_order_relaxed)) {
            return true;
        }
    }
}

[[nodiscard]] inline prepared_minimizer_fragment_reader::packed_occurrence_radix_partition
prepared_minimizer_fragment_reader::partition_packed_occurrence_top_byte_(
    std::span<packed_query_occurrence> occurrences) noexcept {
    /// In-place radix bucket with begin/end pointers into packed occurrences.
    struct bucket {
        packed_query_occurrence* begin = nullptr;
        packed_query_occurrence* end = nullptr;
    };

    DEBUG_ASSERT(!occurrences.empty(), "packed occurrence top-byte partition requires payload");
    packed_query_occurrence* const payload_begin = occurrences.data();
    std::array<bucket, 256u> buckets{};
    for (bucket& bucket_slot : buckets) {
        bucket_slot.begin = payload_begin;
        bucket_slot.end = payload_begin;
    }
    for (packed_query_occurrence& occurrence : occurrences) {
        ++buckets[occurrence.hash >> 24u].end;
    }
    for (std::size_t bucket_index = 1u; bucket_index < buckets.size(); ++bucket_index) {
        buckets[bucket_index].end += buckets[bucket_index - 1u].end - payload_begin;
        buckets[bucket_index].begin = buckets[bucket_index - 1u].end;
    }

    for (std::size_t bucket_ordinal = 0u; bucket_ordinal < buckets.size();) {
        bucket& current_bucket = buckets[bucket_ordinal];
        if (current_bucket.begin == current_bucket.end) {
            ++bucket_ordinal;
            continue;
        }
        bucket* destination = &buckets[current_bucket.begin->hash >> 24u];
        if (destination == &current_bucket) {
            ++current_bucket.begin;
            continue;
        }
        packed_query_occurrence displaced = *current_bucket.begin;
        do {
            const packed_query_occurrence placed = displaced;
            displaced = *destination->begin;
            *destination->begin++ = placed;
            destination = &buckets[displaced.hash >> 24u];
        } while (destination != &current_bucket);
        *current_bucket.begin++ = displaced;
    }

    std::array<std::size_t, 257u> bounds{};
    for (std::size_t bucket_index = 0u; bucket_index < buckets.size(); ++bucket_index) {
        bounds[bucket_index + 1u] =
            static_cast<std::size_t>(buckets[bucket_index].end - payload_begin);
    }
    return bounds;
}

template <class p_visitor>
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
prepared_minimizer_fragment_reader::visit_control_counts_planned_(
    std::size_t entry_ordinal, p_visitor&& visitor) const noexcept {
    DEBUG_ASSERT(entry_ordinal < entry_count_, "block plan entry ordinal is open-validated");
    const grouped_soa_bucket_directory_entry& entry = directory_.entries()[entry_ordinal];
    const entry_plan& plan = entry_plans_[entry_ordinal];
    DEBUG_ASSERT(plan.control_count == entry.control_element_count &&
                     plan.position_element_count == entry.position_element_count &&
                     plan.shape_offset_width == entry.shape_offset_width &&
                     plan.hash_begin <= plan.hash_end && plan.hash_end <= plan.control_end &&
                     plan.count_begin <= plan.count_end && plan.count_end <= plan.control_end &&
                     plan.control_end <= mapped_payload_.size(),
                 "dynamic control scan consumes open-validated geometry");
    const std::size_t count = static_cast<std::size_t>(plan.control_count);

    std::uint64_t previous_hash = 0u;
    std::uint64_t first_hash = 0u;
    std::uint64_t last_hash = 0u;
    for (std::size_t index = 0u; index < count; ++index) {
        const std::uint64_t hash = read_shape_scalar_trusted_(
            mapped_payload_,
            static_cast<std::size_t>(plan.hash_begin) + index * plan.hash_byte_width,
            plan.hash_width);
        const std::uint64_t saturated_count = read_shape_scalar_trusted_(
            mapped_payload_,
            static_cast<std::size_t>(plan.count_begin) + index * plan.count_byte_width,
            plan.count_width);
        if (hash > profile_.max_hash || (index != 0u && hash <= previous_hash) ||
            saturated_count == 0u || saturated_count > profile_.max_occ_cap + 1u) {
            return std::unexpected(minimizer_generation_encoding_error::control_range);
        }
        if (index == 0u)
            first_hash = hash;
        last_hash = hash;
        std::invoke(visitor, hash, saturated_count);
        previous_hash = hash;
    }
    if (first_hash != entry.lo_hash || last_hash != entry.hi_hash) {
        return std::unexpected(minimizer_generation_encoding_error::control_range);
    }
    return {};
}

/// Authenticates grouped-SoA control bytes and invokes a visitor for each decoded control.
template <class p_visitor>
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
prepared_minimizer_fragment_reader::visit_controls_(std::span<const std::byte> input,
                                                    const grouped_soa_bucket_directory_entry& entry,
                                                    const grouped_soa_minimizer_profile& profile,
                                                    p_visitor&& visitor) {
    using namespace minimizer_generation_wire_detail;
    if (profile.layout != minimizer_generation_layout::grouped_soa_segment_local ||
        entry.control_encoded_bytes != input.size() || entry.control_element_count == 0u ||
        entry.control_element_count > (std::numeric_limits<std::size_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::byte_count);
    }
    const auto layout = plan_grouped_soa_control_plane(entry.control_element_count,
                                                       profile.hash_width,
                                                       entry.shape_offset_width,
                                                       profile.count_width);
    if (!layout || layout->encoded_bytes != input.size()) {
        return std::unexpected(minimizer_generation_encoding_error::byte_count);
    }
    const std::size_t count = static_cast<std::size_t>(entry.control_element_count);
    const std::size_t hash_width = static_cast<std::uint8_t>(profile.hash_width);
    const std::size_t offset_width = static_cast<std::uint8_t>(entry.shape_offset_width);
    const std::size_t count_width = static_cast<std::uint8_t>(profile.count_width);
    const std::size_t hash_end = count * hash_width;
    const std::size_t prefix_begin = static_cast<std::size_t>(layout->prefix_offset);
    const std::size_t prefix_end = prefix_begin + (count + 1u) * offset_width;
    const std::size_t count_begin = static_cast<std::size_t>(layout->count_offset);
    const std::size_t count_end = count_begin + count * count_width;
    if (!all_zero(input, hash_end, prefix_begin) || !all_zero(input, prefix_end, count_begin) ||
        !all_zero(input, count_end, input.size())) {
        return std::unexpected(minimizer_generation_encoding_error::reserved_nonzero);
    }

    std::uint64_t previous_hash = 0u;
    std::uint64_t previous_prefix = 0u;
    std::uint64_t first_hash = 0u;
    std::uint64_t last_hash = 0u;
    for (std::size_t index = 0u; index < count; ++index) {
        const auto hash = read_shape_scalar(input, index * hash_width, profile.hash_width);
        const auto prefix =
            read_shape_scalar(input, prefix_begin + index * offset_width, entry.shape_offset_width);
        const auto next_prefix = read_shape_scalar(
            input, prefix_begin + (index + 1u) * offset_width, entry.shape_offset_width);
        const auto saturated_count =
            read_shape_scalar(input, count_begin + index * count_width, profile.count_width);
        if (!hash || !prefix || !next_prefix || !saturated_count || *hash > profile.max_hash ||
            (index != 0u && *hash <= previous_hash) ||
            (index == 0u ? *prefix != 0u : *prefix <= previous_prefix) || *next_prefix <= *prefix ||
            *next_prefix > entry.position_element_count) {
            return std::unexpected(minimizer_generation_encoding_error::control_range);
        }
        const std::uint64_t delta = *next_prefix - *prefix;
        if (*saturated_count != std::min(delta, profile.max_occ_cap + 1u)) {
            return std::unexpected(minimizer_generation_encoding_error::control_range);
        }
        if (index == 0u)
            first_hash = *hash;
        last_hash = *hash;
        std::invoke(visitor,
                    grouped_soa_decoded_control{
                        .hash = *hash,
                        .position_begin = *prefix,
                        .position_end = *next_prefix,
                        .saturated_count = *saturated_count,
                    });
        previous_hash = *hash;
        previous_prefix = *prefix;
    }
    const auto terminal_prefix =
        read_shape_scalar(input, prefix_begin + count * offset_width, entry.shape_offset_width);
    if (!terminal_prefix || *terminal_prefix != entry.position_element_count ||
        first_hash != entry.lo_hash || last_hash != entry.hi_hash) {
        return std::unexpected(minimizer_generation_encoding_error::control_range);
    }
    return {};
}

/// Decodes encoded positions for one control span within a reference core and invokes a
/// visitor with each global encoded position.
template <class p_visitor>
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
prepared_minimizer_fragment_reader::visit_position_range_(
    std::span<const std::byte> input,
    const grouped_soa_decoded_control& control,
    const grouped_soa_bucket_directory_entry& entry,
    const minimizer_position_segment_bounds& core,
    const grouped_soa_minimizer_profile& profile,
    p_visitor&& visitor) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (control.position_end <= control.position_begin ||
        control.position_end > entry.position_element_count) {
        return std::unexpected(minimizer_generation_encoding_error::control_range);
    }
    const std::size_t width = static_cast<std::uint8_t>(profile.position_width);
    if (width == 0u ||
        entry.position_element_count > (std::numeric_limits<std::size_t>::max)() / width ||
        input.size() != static_cast<std::size_t>(entry.position_element_count) * width) {
        return std::unexpected(minimizer_generation_encoding_error::byte_count);
    }
    std::optional<std::uint64_t> previous;
    for (std::uint64_t ordinal = control.position_begin; ordinal < control.position_end;
         ++ordinal) {
        const std::uint64_t local = read_shape_scalar_trusted_(
            input, static_cast<std::size_t>(ordinal) * width, profile.position_width);
        const std::uint64_t local_bp = local >> 1u;
        if (local_bp >= core.end_bp - core.begin_bp ||
            core.begin_bp > (std::numeric_limits<std::uint64_t>::max)() - local_bp) {
            return std::unexpected(minimizer_generation_encoding_error::position_range);
        }
        const std::uint64_t global_bp = core.begin_bp + local_bp;
        if (global_bp > ((std::numeric_limits<std::uint64_t>::max)() >> 1u)) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        const std::uint64_t encoded = (global_bp << 1u) | (local & 1u);
        if (previous && encoded <= *previous) {
            return std::unexpected(minimizer_generation_encoding_error::position_range);
        }
        std::invoke(visitor, encoded);
        previous = encoded;
    }
    return {};
}

/// Maps a packed target position and query minimizer sketch into one chaining anchor.
[[nodiscard]] inline std::expected<bioseq::chaining::anchor, minimizer_generation_encoding_error>
prepared_minimizer_fragment_reader::make_fragment_anchor_(std::uint64_t packed_target,
                                                          const minimap_sketch_data& minimizer,
                                                          sequence_size_t query_length,
                                                          std::uint8_t query_span) noexcept {
    const std::uint64_t ref_position = packed_target >> 1u;
    const std::uint32_t target_strand = static_cast<std::uint32_t>(packed_target & 1u);
    const std::uint64_t encoded = minimizer.pos_strand_encoded();
    const std::uint32_t query_strand = static_cast<std::uint32_t>(encoded & 1u);
    const std::uint64_t forward_last = encoded >> 1u;
    if (ref_position >= (std::uint64_t{1} << 63u) || query_span == 0u ||
        query_length < query_span || forward_last >= query_length ||
        forward_last + 1u < query_span) {
        return std::unexpected(minimizer_generation_encoding_error::position_range);
    }
    const std::uint64_t kmer_start = forward_last + 1u - query_span;
    const std::uint64_t reverse_last = query_length - 1u - kmer_start;
    const std::uint32_t reverse = target_strand ^ query_strand;
    return bioseq::chaining::anchor{
        .ref_position = ref_position,
        .query_position = reverse != 0u ? reverse_last : forward_last,
        .is_reverse = reverse,
        .query_span = query_span,
        .flags = 0u,
        .segment_id = 0u,
    };
}

/// Scans trusted control counts for one block entry range and invokes a visitor for
/// each matched query group.
template <bool p_oracle, class p_visitor>
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
prepared_minimizer_fragment_reader::visit_matched_control_counts_(
    const fragment_block_plan& block,
    fragment_probe_context& context,
    p_visitor&& visitor) const noexcept {
    if constexpr (p_oracle) {
        DEBUG_ASSERT(context.query_plan_radix.size() ==
                         static_cast<std::size_t>(query_radix_bucket_count_) + 1u,
                     "oracle query radix is prevalidated");
    } else {
        DEBUG_ASSERT(context.bucket_direct_matcher != nullptr,
                     "production matcher view is prevalidated");
    }
    DEBUG_ASSERT(block.entry_begin < block.entry_end && block.entry_end <= entry_count_,
                 "dynamic count scan consumes an open-validated block");
    for (std::size_t ordinal = block.entry_begin; ordinal < block.entry_end; ++ordinal) {
        std::uint32_t active_bucket = (std::numeric_limits<std::uint32_t>::max)();
        std::uint32_t plan_cursor = 0u;
        std::uint32_t plan_end = 0u;
        std::optional<minimizer_generation_encoding_error> callback_error;
        const auto decoded = visit_control_counts_planned_(
            ordinal,
            [this, &callback_error, &active_bucket, &plan_cursor, &plan_end, &context, &visitor](
                std::uint64_t hash, std::uint64_t local_count) noexcept {
                if (callback_error)
                    return;
                const std::uint32_t hash32 = static_cast<std::uint32_t>(hash);
                const auto matched_group = match_query_group_<p_oracle>(
                    hash32, context, active_bucket, plan_cursor, plan_end);
                if (!matched_group)
                    return;
                callback_error = std::invoke(visitor, *matched_group, local_count);
            });
        if (!decoded)
            return std::unexpected(decoded.error());
        if (callback_error) {
            return std::unexpected(*callback_error);
        }
    }
    return {};
}

/**
 * Scans matched controls in a block with prefix authentication.
 *
 * Optionally accumulates accepted or rejected prefix byte totals and can validate
 * rejected-prefix geometry without walking positions.
 */
template <bool p_oracle, class p_visitor>
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
prepared_minimizer_fragment_reader::visit_matched_controls_(
    const fragment_block_plan& block,
    fragment_probe_context& context,
    p_visitor&& visitor,
    std::uint64_t* accepted_prefix_bytes,
    std::uint64_t* rejected_prefix_bytes,
    bool validate_rejected_prefixes) const noexcept {
    using namespace minimizer_generation_wire_detail;
    if constexpr (p_oracle) {
        DEBUG_ASSERT(context.query_plan_radix.size() ==
                         static_cast<std::size_t>(query_radix_bucket_count_) + 1u,
                     "oracle query radix is prevalidated");
    } else {
        DEBUG_ASSERT(context.bucket_direct_matcher != nullptr,
                     "production matcher view is prevalidated");
    }
    DEBUG_ASSERT(block.entry_begin < block.entry_end && block.entry_end <= entry_count_,
                 "dynamic matched scan consumes an open-validated block");
    for (std::size_t ordinal = block.entry_begin; ordinal < block.entry_end; ++ordinal) {
        const entry_plan& selected_entry = entry_plans_[ordinal];
        const std::size_t offset_width = selected_entry.offset_byte_width;
        std::uint32_t active_bucket = (std::numeric_limits<std::uint32_t>::max)();
        std::uint32_t plan_cursor = 0u;
        std::uint32_t plan_end = 0u;
        std::optional<minimizer_generation_encoding_error> callback_error;
        std::optional<std::uint64_t> previous_accepted_position_end;
        std::size_t control_ordinal = 0u;
        const auto decoded = visit_control_counts_planned_(
            ordinal,
            [this,
             &control_ordinal,
             &callback_error,
             &active_bucket,
             &plan_cursor,
             &plan_end,
             &context,
             offset_width,
             &selected_entry,
             &previous_accepted_position_end,
             &visitor,
             accepted_prefix_bytes,
             rejected_prefix_bytes,
             validate_rejected_prefixes,
             ordinal](std::uint64_t hash, std::uint64_t saturated_count) noexcept {
                const std::size_t current_ordinal = control_ordinal++;
                if (callback_error)
                    return;
                const std::uint32_t hash32 = static_cast<std::uint32_t>(hash);
                const auto matched_group = match_query_group_<p_oracle>(
                    hash32, context, active_bucket, plan_cursor, plan_end);
                if (!matched_group)
                    return;
                const std::uint32_t global_count =
                    occurrence_counter_<p_oracle>(context, matched_group->counter_ordinal)
                        .load(std::memory_order_relaxed);
                if (global_count == 0u) {
                    callback_error = minimizer_generation_encoding_error::partition_id;
                    return;
                }
                // Pass 3 suppresses a rejected control before either offset
                // endpoint is read. Rejected endpoints were authenticated
                // exactly once by Pass 2 (or the rejected-only rescan).
                if (global_count > context.max_occ && !validate_rejected_prefixes) {
                    return;
                }
                std::uint64_t* const prefix_stats =
                    global_count > context.max_occ ? rejected_prefix_bytes : accepted_prefix_bytes;
                if (prefix_stats != nullptr &&
                    !add_checked_(*prefix_stats, 2u * static_cast<std::uint64_t>(offset_width))) {
                    callback_error = minimizer_generation_encoding_error::arithmetic_overflow;
                    return;
                }
                const std::size_t prefix_offset =
                    static_cast<std::size_t>(selected_entry.prefix_begin) +
                    current_ordinal * offset_width;
                const std::uint64_t position_begin = read_shape_scalar_trusted_(
                    mapped_payload_, prefix_offset, selected_entry.shape_offset_width);
                const std::uint64_t position_end =
                    read_shape_scalar_trusted_(mapped_payload_,
                                               prefix_offset + offset_width,
                                               selected_entry.shape_offset_width);
                if (position_end <= position_begin ||
                    position_end > selected_entry.position_element_count ||
                    (current_ordinal == 0u && position_begin != 0u) ||
                    (current_ordinal + 1u == selected_entry.control_count &&
                     position_end != selected_entry.position_element_count) ||
                    (previous_accepted_position_end &&
                     position_begin < *previous_accepted_position_end)) {
                    callback_error = minimizer_generation_encoding_error::control_range;
                    return;
                }
                previous_accepted_position_end = position_end;
                const std::uint64_t delta = position_end - position_begin;
                if (saturated_count != std::min(delta, profile_.max_occ_cap + 1u)) {
                    callback_error = minimizer_generation_encoding_error::control_range;
                    return;
                }
                // A globally rejected query hash still names a selected
                // control. Authenticate its prefix geometry and saturated
                // count before suppressing the position walk; otherwise a
                // corrupt heavy control could hide behind max-occ pruning.
                if (global_count > context.max_occ) {
                    return;
                }
                callback_error = std::invoke(visitor,
                                             ordinal,
                                             *matched_group,
                                             grouped_soa_decoded_control{
                                                 .hash = hash,
                                                 .position_begin = position_begin,
                                                 .position_end = position_end,
                                                 .saturated_count = saturated_count,
                                             });
            });
        if (!decoded)
            return std::unexpected(decoded.error());
        if (callback_error) {
            return std::unexpected(*callback_error);
        }
    }
    return {};
}

/**
 * Looks up the query occurrence group for a hash.
 *
 * \tparam p_oracle Uses oracle scratch radix plans instead of the production matcher view.
 */
template <bool p_oracle>
[[nodiscard, gnu::always_inline]] inline std::optional<
    prepared_minimizer_fragment_reader::query_occurrence_group>
prepared_minimizer_fragment_reader::match_query_group_(std::uint32_t hash,
                                                       const fragment_probe_context& context,
                                                       std::uint32_t& active_bucket,
                                                       std::uint32_t& plan_cursor,
                                                       std::uint32_t& plan_end) const noexcept {
    const std::uint32_t bucket = query_bucket_(hash);
    if constexpr (p_oracle) {
        DEBUG_ASSERT(context.scratch != nullptr &&
                         context.query_plan_radix.size() ==
                             static_cast<std::size_t>(query_radix_bucket_count_) + 1u,
                     "oracle matcher owners are prevalidated");
        if (bucket != active_bucket) {
            active_bucket = bucket;
            plan_cursor = context.query_plan_radix[bucket];
            plan_end = context.query_plan_radix[bucket + 1u];
        }
        DEBUG_ASSERT(plan_cursor <= plan_end && plan_end <= context.plans.size(),
                     "oracle matcher bucket range is prevalidated");
        while (plan_cursor < plan_end && context.plans[plan_cursor].hash < hash) {
            ++plan_cursor;
        }
        if (plan_cursor == plan_end || context.plans[plan_cursor].hash != hash) {
            return std::nullopt;
        }
        const std::uint32_t matched_plan = plan_cursor++;
        const query_wave_scratch::query_hash_plan& plan = context.plans[matched_plan];
        return query_occurrence_group{
            .hash = plan.hash,
            .occurrence_ref_begin = plan.occurrence_ref_begin,
            .occurrence_ref_count = plan.occurrence_ref_count,
            .counter_ordinal = matched_plan,
        };
    } else {
        DEBUG_ASSERT(context.bucket_direct_matcher != nullptr,
                     "production matcher view is prevalidated");
        const bucket_direct_matcher_view& view = *context.bucket_direct_matcher;
        const std::size_t filter_word = query_membership_filter_word_(hash);
        DEBUG_ASSERT(filter_word < view.membership_filter_word_count,
                     "production filter projection is prevalidated");
        const std::uint64_t required_mask = query_membership_filter_mask_(hash);
        const std::uint64_t present_mask = view.membership_filter[filter_word];
        if ((present_mask & required_mask) != required_mask) {
            return std::nullopt;
        }

        DEBUG_ASSERT(static_cast<std::size_t>(bucket) + 1u < view.radix_count,
                     "production radix projection is prevalidated");
        const std::uint32_t group_begin = view.radix[bucket];
        const std::uint32_t group_end = view.radix[bucket + 1u];
        DEBUG_ASSERT(group_begin <= group_end &&
                         static_cast<std::size_t>(group_end) + 1u <= view.group_offset_count,
                     "production group directory is prevalidated");
        const auto exact_group =
            [&view, hash, group_begin, group_end](
                const auto* occurrences,
                std::size_t occurrence_count) noexcept -> std::optional<query_occurrence_group> {
            const std::uint32_t* const first = view.group_offsets + group_begin;
            const std::uint32_t* const last = view.group_offsets + group_end;
            const auto found = std::lower_bound(
                first,
                last,
                hash,
                [occurrences, occurrence_count](std::uint32_t occurrence_ordinal,
                                                std::uint32_t query_hash) noexcept {
                    DEBUG_ASSERT(occurrence_ordinal < occurrence_count,
                                 "production group offset is prevalidated");
                    return occurrences[occurrence_ordinal].hash < query_hash;
                });
            if (found == last || occurrences[*found].hash != hash) {
                return std::nullopt;
            }
            const std::uint32_t occurrence_begin = *found;
            const std::uint32_t occurrence_end = *(found + 1u);
            DEBUG_ASSERT(occurrence_end > occurrence_begin && occurrence_end <= occurrence_count,
                         "production group extent is prevalidated");
            return query_occurrence_group{
                .hash = hash,
                .occurrence_ref_begin = occurrence_begin,
                .occurrence_ref_count = occurrence_end - occurrence_begin,
                .counter_ordinal = static_cast<std::uint32_t>(found - view.group_offsets),
            };
        };
        switch (view.occurrence_layout.route) {
        case query_occurrence_route::packed:
            return exact_group(view.packed_occurrences, view.packed_occurrence_count);
        case query_occurrence_route::wide:
            return exact_group(view.wide_occurrences, view.wide_occurrence_count);
        }
        return std::nullopt;
    }
}

/// Tallies forward and reverse anchor counts for every occurrence in one hash group.
template <bool p_oracle>
[[nodiscard]] inline std::optional<minimizer_generation_encoding_error>
prepared_minimizer_fragment_reader::accumulate_orientation_for_group_(
    const fragment_probe_context& context,
    const query_occurrence_group& group,
    std::uint64_t target_strand_zero,
    std::uint64_t target_strand_one) noexcept {
    DEBUG_ASSERT(context.scratch != nullptr && !context.queries.empty() &&
                     context.block_ordinal <=
                         (std::numeric_limits<std::size_t>::max)() / context.queries.size(),
                 "orientation occurrence context is prevalidated");
    const orientation_occurrence_context occurrence_context{
        .queries = context.queries,
        .scratch = context.scratch,
        .layout_base = context.block_ordinal * context.queries.size(),
        .target_strand_zero = target_strand_zero,
        .target_strand_one = target_strand_one,
    };
    return with_query_group_occurrences_<p_oracle>(
        context,
        group,
        [&occurrence_context](const auto occurrences,
                              const query_occurrence_packing_layout& packing_layout) noexcept
            -> std::optional<minimizer_generation_encoding_error> {
            for (const auto& occurrence : occurrences) {
                if (const auto error = accumulate_orientation_(
                        query_occurrence_reference_(occurrence, packing_layout),
                        occurrence_context)) {
                    return error;
                }
            }
            return std::nullopt;
        });
}

/// Counts strand orientations from fixed p16 positions and updates layout counts for one
/// matched group.
template <bool p_oracle>
[[nodiscard]] inline std::optional<minimizer_generation_encoding_error>
prepared_minimizer_fragment_reader::process_fixed_orientation_control_(
    fragment_probe_context& context,
    const entry_plan& entry,
    const query_occurrence_group& group,
    const grouped_soa_decoded_control& control) const noexcept {
    DEBUG_ASSERT(context.position_bytes != nullptr &&
                     entry.position_width == minimizer_scalar_width::u16,
                 "fixed orientation context is prevalidated");
    auto cursor = position_range_cursor::create(mapped_payload_, control, entry);
    if (!cursor)
        return cursor.error();
    std::uint64_t target_strand_zero = 0u;
    std::uint64_t target_strand_one = 0u;
    std::uint64_t packed = 0u;
    while (cursor->next_fixed_p16(packed)) {
        std::uint64_t& count = (packed & 1u) == 0u ? target_strand_zero : target_strand_one;
        if (count == (std::numeric_limits<std::uint64_t>::max)()) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        ++count;
    }
    if (const auto error = cursor->error())
        return error;
    std::uint64_t observed = target_strand_zero;
    if (!add_checked_(observed, target_strand_one) ||
        observed != control.position_end - control.position_begin ||
        !add_checked_(*context.position_bytes, observed * sizeof(std::uint16_t))) {
        return minimizer_generation_encoding_error::arithmetic_overflow;
    }
    return accumulate_orientation_for_group_<p_oracle>(
        context, group, target_strand_zero, target_strand_one);
}

/// Emits fragment anchors from fixed p16 positions for every occurrence in one matched
/// group.
template <bool p_oracle>
[[nodiscard]] inline std::optional<minimizer_generation_encoding_error>
prepared_minimizer_fragment_reader::process_fixed_emission_control_(
    fragment_probe_context& context,
    const entry_plan& entry,
    const query_occurrence_group& group,
    const grouped_soa_decoded_control& control) const noexcept {
    DEBUG_ASSERT(context.scratch != nullptr && context.batch != nullptr &&
                     context.block != nullptr && context.position_bytes != nullptr &&
                     context.anchors_constructed != nullptr &&
                     entry.position_width == minimizer_scalar_width::u16,
                 "fixed emission context is prevalidated");
    auto cursor = position_range_cursor::create(mapped_payload_, control, entry);
    if (!cursor)
        return cursor.error();
    const direct_emission_occurrence_context occurrence_context{
        .queries = context.queries,
        .block = context.block,
        .block_ordinal = context.block_ordinal,
        .scratch = context.scratch,
        .batch = context.batch,
    };

    const auto occurrence_error = with_query_group_occurrences_<p_oracle>(
        context,
        group,
        [this, &cursor, &occurrence_context](
            const auto occurrences, const query_occurrence_packing_layout& packing_layout) noexcept
            -> std::optional<minimizer_generation_encoding_error> {
            std::uint64_t packed = 0u;
            while (cursor->next_fixed_p16(packed)) {
                for (const auto& occurrence : occurrences) {
                    if (const auto error = emit_fragment_anchor_(
                            packed,
                            query_occurrence_reference_(occurrence, packing_layout),
                            occurrence_context)) {
                        return error;
                    }
                }
            }
            return cursor->error();
        });
    if (occurrence_error)
        return occurrence_error;

    const std::uint64_t decoded_positions = control.position_end - control.position_begin;
    const std::uint64_t position_bytes =
        bioseq::common::saturating_mul(decoded_positions, sizeof(std::uint16_t));
    const std::uint64_t constructed =
        bioseq::common::saturating_mul(decoded_positions, group.occurrence_ref_count);
    if (position_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        constructed == (std::numeric_limits<std::uint64_t>::max)() ||
        !add_checked_(*context.position_bytes, position_bytes) ||
        !add_checked_(*context.anchors_constructed, constructed)) {
        return minimizer_generation_encoding_error::arithmetic_overflow;
    }
    return std::nullopt;
}

/// Pass-1 probe that reserves occurrence counters for matched hashes in one H32/C16 entry.
template <bool p_oracle>
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
prepared_minimizer_fragment_reader::probe_entry_h32_c16_count_(
    fragment_probe_context& context, std::size_t entry_ordinal) const noexcept {
    DEBUG_ASSERT(context.scratch != nullptr && entry_ordinal < entry_count_ &&
                     context.pass == fragment_probe_pass::control_count,
                 "fixed count context is prevalidated");
    if constexpr (p_oracle) {
        DEBUG_ASSERT(context.query_plan_radix.size() ==
                         static_cast<std::size_t>(query_radix_bucket_count_) + 1u,
                     "oracle query radix is prevalidated");
    } else {
        DEBUG_ASSERT(context.bucket_direct_matcher != nullptr,
                     "production matcher view is prevalidated");
    }
    const grouped_soa_bucket_directory_entry& directory_entry = directory_.entries()[entry_ordinal];
    const entry_plan& entry = entry_plans_[entry_ordinal];
    DEBUG_ASSERT(
        entry.hash_width == minimizer_scalar_width::u32 &&
            entry.count_width == minimizer_scalar_width::u16 &&
            entry.position_width == minimizer_scalar_width::u16 &&
            entry.control_count == directory_entry.control_element_count &&
            entry.position_element_count == directory_entry.position_element_count &&
            entry.control_begin <= entry.hash_begin && entry.hash_begin <= entry.hash_end &&
            entry.hash_end <= entry.prefix_begin && entry.prefix_begin <= entry.prefix_end &&
            entry.prefix_end <= entry.count_begin && entry.count_begin <= entry.count_end &&
            entry.count_end <= entry.control_end && entry.control_end <= mapped_payload_.size(),
        "fixed probe consumes open-validated entry geometry");

    std::uint32_t active_bucket = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t plan_cursor = 0u;
    std::uint32_t plan_end = 0u;
    std::uint32_t previous_hash = 0u;
    std::uint32_t first_hash = 0u;
    std::uint32_t last_hash = 0u;
    const std::size_t count = static_cast<std::size_t>(entry.control_count);
    for (std::size_t control_ordinal = 0u; control_ordinal < count; ++control_ordinal) {
        const std::uint32_t hash =
            static_cast<std::uint32_t>(read_fixed_width_le_<minimizer_scalar_width::u32>(
                mapped_payload_,
                static_cast<std::size_t>(entry.hash_begin) +
                    control_ordinal * sizeof(std::uint32_t)));
        const std::uint64_t local_count = read_fixed_width_le_<minimizer_scalar_width::u16>(
            mapped_payload_,
            static_cast<std::size_t>(entry.count_begin) + control_ordinal * sizeof(std::uint16_t));
        if (hash > profile_.max_hash || (control_ordinal != 0u && hash <= previous_hash) ||
            local_count == 0u || local_count > profile_.max_occ_cap + 1u) {
            return std::unexpected(minimizer_generation_encoding_error::control_range);
        }
        if (control_ordinal == 0u)
            first_hash = hash;
        last_hash = hash;
        previous_hash = hash;

        const auto matched_group =
            match_query_group_<p_oracle>(hash, context, active_bucket, plan_cursor, plan_end);
        if (!matched_group)
            continue;
        (void)reserve_query_occurrences_(
            occurrence_counter_<p_oracle>(context, matched_group->counter_ordinal),
            local_count,
            context.max_occ);
    }
    if (first_hash != directory_entry.lo_hash || last_hash != directory_entry.hi_hash) {
        return std::unexpected(minimizer_generation_encoding_error::control_range);
    }
    return {};
}

/**
 * Matched-control probe for fixed H32/C16/P16 entries.
 *
 * \tparam p_offset_width Prefix offset width for the entry shape.
 */
template <bool p_oracle, minimizer_scalar_width p_offset_width>
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
prepared_minimizer_fragment_reader::probe_entry_h32_c16_p16_matched_(
    fragment_probe_context& context, std::size_t entry_ordinal) const noexcept {
    static_assert(p_offset_width == minimizer_scalar_width::u16 ||
                  p_offset_width == minimizer_scalar_width::u32 ||
                  p_offset_width == minimizer_scalar_width::u64);
    DEBUG_ASSERT(context.scratch != nullptr && entry_ordinal < entry_count_ &&
                     context.pass != fragment_probe_pass::control_count &&
                     context.matched_count_bytes != nullptr,
                 "fixed matched context is prevalidated");
    if constexpr (p_oracle) {
        DEBUG_ASSERT(context.query_plan_radix.size() ==
                         static_cast<std::size_t>(query_radix_bucket_count_) + 1u,
                     "oracle query radix is prevalidated");
    } else {
        DEBUG_ASSERT(context.bucket_direct_matcher != nullptr,
                     "production matcher view is prevalidated");
    }
    const entry_plan& entry = entry_plans_[entry_ordinal];
    DEBUG_ASSERT(
        entry.shape_offset_width == p_offset_width &&
            entry.hash_width == minimizer_scalar_width::u32 &&
            entry.count_width == minimizer_scalar_width::u16 &&
            entry.position_width == minimizer_scalar_width::u16 &&
            entry.control_count == directory_.entries()[entry_ordinal].control_element_count &&
            entry.position_element_count ==
                directory_.entries()[entry_ordinal].position_element_count &&
            entry.control_begin <= entry.hash_begin && entry.hash_begin <= entry.hash_end &&
            entry.hash_end <= entry.prefix_begin && entry.prefix_begin <= entry.prefix_end &&
            entry.prefix_end <= entry.count_begin && entry.count_begin <= entry.count_end &&
            entry.count_end <= entry.control_end && entry.control_end <= mapped_payload_.size(),
        "fixed matched probe consumes Pass1-authenticated controls");

    std::uint32_t active_bucket = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t plan_cursor = 0u;
    std::uint32_t plan_end = 0u;
    std::optional<std::uint64_t> previous_selected_position_end;
    const std::size_t count = static_cast<std::size_t>(entry.control_count);
    for (std::size_t control_ordinal = 0u; control_ordinal < count; ++control_ordinal) {
        // Pass 1 has already authenticated H ordering/range and every C
        // value under a joined barrier. Later mandatory passes scan H for
        // matching, then touch C only for a control they will consume.
        const std::uint32_t hash =
            static_cast<std::uint32_t>(read_fixed_width_le_<minimizer_scalar_width::u32>(
                mapped_payload_,
                static_cast<std::size_t>(entry.hash_begin) +
                    control_ordinal * sizeof(std::uint32_t)));
        const auto matched_group =
            match_query_group_<p_oracle>(hash, context, active_bucket, plan_cursor, plan_end);
        if (!matched_group)
            continue;
        const std::uint32_t global_count =
            occurrence_counter_<p_oracle>(context, matched_group->counter_ordinal)
                .load(std::memory_order_relaxed);
        if (global_count == 0u) {
            return std::unexpected(minimizer_generation_encoding_error::partition_id);
        }
        if (global_count > context.max_occ && context.pass == fragment_probe_pass::emission) {
            continue;
        }
        if (!add_checked_(*context.matched_count_bytes, sizeof(std::uint16_t))) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        const std::uint64_t local_count = read_fixed_width_le_<minimizer_scalar_width::u16>(
            mapped_payload_,
            static_cast<std::size_t>(entry.count_begin) + control_ordinal * sizeof(std::uint16_t));
        std::uint64_t* const prefix_stats = global_count > context.max_occ
                                                ? context.rejected_prefix_bytes
                                                : context.accepted_prefix_bytes;
        if (prefix_stats != nullptr &&
            !add_checked_(*prefix_stats, 2u * sizeof(minimizer_wire_uint_t_<p_offset_width>))) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        const std::size_t prefix_offset =
            static_cast<std::size_t>(entry.prefix_begin) +
            control_ordinal * sizeof(minimizer_wire_uint_t_<p_offset_width>);
        const std::uint64_t position_begin =
            read_fixed_width_le_<p_offset_width>(mapped_payload_, prefix_offset);
        const std::uint64_t position_end = read_fixed_width_le_<p_offset_width>(
            mapped_payload_, prefix_offset + sizeof(minimizer_wire_uint_t_<p_offset_width>));
        if (position_end <= position_begin || position_end > entry.position_element_count ||
            (control_ordinal == 0u && position_begin != 0u) ||
            (control_ordinal + 1u == count && position_end != entry.position_element_count) ||
            (previous_selected_position_end && position_begin < *previous_selected_position_end) ||
            local_count != std::min(position_end - position_begin, profile_.max_occ_cap + 1u)) {
            return std::unexpected(minimizer_generation_encoding_error::control_range);
        }
        previous_selected_position_end = position_end;

        if (global_count > context.max_occ) {
            if (context.pass == fragment_probe_pass::rejected_validation ||
                context.pass == fragment_probe_pass::orientation) {
                continue;
            }
            return std::unexpected(minimizer_generation_encoding_error::partition_id);
        }
        if (context.pass == fragment_probe_pass::rejected_validation) {
            return std::unexpected(minimizer_generation_encoding_error::partition_id);
        }
        const grouped_soa_decoded_control control{
            .hash = hash,
            .position_begin = position_begin,
            .position_end = position_end,
            .saturated_count = local_count,
        };
        const auto error = context.pass == fragment_probe_pass::orientation
                               ? process_fixed_orientation_control_<p_oracle>(
                                     context, entry, *matched_group, control)
                               : process_fixed_emission_control_<p_oracle>(
                                     context, entry, *matched_group, control);
        if (error)
            return std::unexpected(*error);
    }
    return {};
}

} // namespace bioseq::indexing
