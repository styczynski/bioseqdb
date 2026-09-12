/**
 * @file compact_direct/reader/direct_execution.cppm
 * @brief Compact-direct selection traversal and exact anchor emission.
 */
module;

#include <new>

export module bioseq.indexing.minimizer_hashmap:compact_direct.reader_direct_execution;

import :compact_direct.reader_contract;
import :compact_direct.reader_codec;
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

/// Builds one chaining anchor from a packed target position and stores it in the shard
/// query layout slice for compact-direct emission.
[[nodiscard]] inline std::optional<minimizer_generation_encoding_error>
prepared_minimizer_fragment_reader::emit_compact_direct_anchor_(
    std::uint64_t packed_target_position,
    query_occurrence_reference occurrence,
    const compact_direct_emission_occurrence_context& context) noexcept {
    if (context.scratch == nullptr || context.batch == nullptr ||
        occurrence.batch_query_ordinal >= context.queries.size() ||
        occurrence.query_ordinal >=
            context.queries[occurrence.batch_query_ordinal].minimizers.size()) {
        return minimizer_generation_encoding_error::partition_id;
    }
    const std::size_t query_ordinal = occurrence.batch_query_ordinal;
    const auto position_strand = query_position_strand_(occurrence, context.queries);
    if (!position_strand) {
        return position_strand.error();
    }
    const auto anchor =
        make_fragment_anchor_(packed_target_position,
                              *position_strand,
                              context.queries[query_ordinal].query_length,
                              static_cast<std::uint8_t>(context.queries[query_ordinal].kmer_size));
    if (!anchor || anchor->ref_position >= context.batch->target_length_) {
        return anchor ? minimizer_generation_encoding_error::position_range : anchor.error();
    }
    if (context.layout_base > context.scratch->block_query_layouts_.size() ||
        query_ordinal >= context.scratch->block_query_layouts_.size() - context.layout_base ||
        query_ordinal >= context.batch->query_count_) {
        return minimizer_generation_encoding_error::arithmetic_overflow;
    }
    block_query_anchor_layout& layout =
        context.scratch->block_query_layouts_[context.layout_base + query_ordinal];
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

/// Validates compact-hash32 selection spans and binds the query occurrence group from
/// the query hash plan. Sets context.error on failure.
template <class p_context>
[[nodiscard]] inline bool prepared_minimizer_fragment_reader::bind_compact_direct_selection_(
    p_context& context,
    const compact_hash32_selection_posting_spans& selected,
    query_occurrence_group& group) noexcept {
    if (context.error || context.reader == nullptr || context.plan == nullptr ||
        context.scratch == nullptr ||
        selected.selection_ordinal >= context.plan->selections.size() ||
        selected.global_count == 0u || selected.strand_zero_count > selected.global_count ||
        selected.span_count == 0u || selected.span_count > selected.spans.size()) {
        context.error = minimizer_generation_encoding_error::partition_id;
        return false;
    }
    const compact_hash32_unique_selection& selection =
        context.plan->selections[selected.selection_ordinal];
    if (selected.dense_ordinal != selection.dense_ordinal ||
        selected.query_ordinal != selection.query_ordinal ||
        selection.query_ordinal >= context.scratch->query_plans_.size()) {
        context.error = minimizer_generation_encoding_error::partition_id;
        return false;
    }

    std::uint64_t span_postings = 0u;
    std::uint64_t strand_zero_postings = 0u;
    bool have_previous_strand = false;
    bool previous_strand_one = false;
    for (std::uint8_t span_ordinal = 0u; span_ordinal < selected.span_count; ++span_ordinal) {
        const compact_hash32_posting_span_view& span = selected.spans[span_ordinal];
        if (span.count == 0u || span.posting_width_bytes != context.plan->posting_width_bytes ||
            span.target_length != context.reader->manifest_.target_length ||
            (have_previous_strand && previous_strand_one >= span.target_strand_one) ||
            !add_checked_(span_postings, span.count) ||
            (!span.target_strand_one && !add_checked_(strand_zero_postings, span.count))) {
            context.error = minimizer_generation_encoding_error::partition_id;
            return false;
        }
        previous_strand_one = span.target_strand_one;
        have_previous_strand = true;
    }
    if (span_postings != selected.global_count ||
        strand_zero_postings != selected.strand_zero_count) {
        context.error = minimizer_generation_encoding_error::element_count;
        return false;
    }

    // Compact selections deliberately retain only dense and query
    // ordinals. The query-wave owner remains the single hash authority.
    const query_wave_scratch::query_hash_plan& query_plan =
        context.scratch->query_plans_[selection.query_ordinal];
    if (query_plan.occurrence_ref_count == 0u) {
        context.error = minimizer_generation_encoding_error::partition_id;
        return false;
    }
    group = query_occurrence_group{
        .hash = query_plan.hash,
        .occurrence_ref_begin = query_plan.occurrence_ref_begin,
        .occurrence_ref_count = query_plan.occurrence_ref_count,
        .counter_ordinal = selection.query_ordinal,
    };
    return true;
}

/// Accumulates inverted-index page advice for each posting span in one selected hash.
[[nodiscard]] inline bool
prepared_minimizer_fragment_reader::accumulate_compact_direct_selected_page_advice_(
    compact_direct_layout_pass_context& context,
    const compact_hash32_selection_posting_spans& selected) noexcept {
    if (context.reader == nullptr || !context.reader->compact_direct_) {
        context.error = minimizer_generation_encoding_error::partition_id;
        return false;
    }
    const compact_direct_selected_page_adviser adviser = context.reader->selected_page_adviser_;
    if (adviser.empty())
        return true;

    for (std::uint8_t span_ordinal = 0u; span_ordinal < selected.span_count; ++span_ordinal) {
        const auto page_range =
            compact_direct_select_page_range(adviser,
                                             context.reader->compact_direct_->view(),
                                             selected.spans[span_ordinal].encoded_bytes);
        if (!page_range) {
            context.direct_error = page_range.error();
            return false;
        }
        if (!*page_range)
            continue;

        if (!context.selected_page_advice.accumulate(adviser, **page_range)) {
            context.direct_error = direct_inverted_index_error::query_plan;
            return false;
        }
    }
    return true;
}

/// Layout-pass selection visitor that binds the group, accumulates page advice, and
/// tallies orientation counts for the selected postings.
[[nodiscard]] inline bool
prepared_minimizer_fragment_reader::visit_compact_direct_layout_selection_(
    compact_direct_layout_pass_context& context,
    const compact_hash32_selection_posting_spans& selected) noexcept {
    query_occurrence_group group{};
    if (!bind_compact_direct_selection_(context, selected, group)) {
        return false;
    }
    if (!accumulate_compact_direct_selected_page_advice_(context, selected)) {
        return false;
    }
    fragment_probe_context occurrence_context{
        .block_ordinal = context.shard_ordinal,
        .queries = context.queries,
        .scratch = context.scratch,
    };
    context.error =
        accumulate_orientation_for_group_<true>(occurrence_context,
                                                group,
                                                selected.strand_zero_count,
                                                selected.global_count - selected.strand_zero_count);
    if (context.error || !add_checked_(context.logical_postings_counted, selected.global_count)) {
        if (!context.error) {
            context.error = minimizer_generation_encoding_error::arithmetic_overflow;
        }
        return false;
    }
    return true;
}

/// Emission-pass selection visitor that decodes compact-direct posting spans and writes
/// fragment anchors into the prepared batch.
[[nodiscard]] inline bool
prepared_minimizer_fragment_reader::visit_compact_direct_emission_selection_(
    compact_direct_emission_pass_context& context,
    const compact_hash32_selection_posting_spans& selected) noexcept {
    if (context.direct_error || context.batch == nullptr) {
        context.error = minimizer_generation_encoding_error::partition_id;
        return false;
    }
    query_occurrence_group group{};
    if (!bind_compact_direct_selection_(context, selected, group)) {
        return false;
    }
    const compact_direct_emission_occurrence_context emission_context{
        .queries = context.queries,
        .scratch = context.scratch,
        .batch = context.batch,
        .layout_base = context.shard_ordinal * context.queries.size(),
    };
    const fragment_probe_context occurrence_context{
        .block_ordinal = context.shard_ordinal,
        .queries = context.queries,
        .scratch = context.scratch,
        .batch = context.batch,
    };

    // Bind the packed/wide occurrence representation once per selected
    // hash. The fixed occurrence span then stays in registers while all of
    // that descriptor's postings are decoded and expanded into anchors.
    context.error = with_query_group_occurrences_<true>(
        occurrence_context,
        group,
        [&context, &selected, &emission_context, &group](
            const auto occurrences, const query_occurrence_packing_layout& packing_layout) noexcept
            -> std::optional<minimizer_generation_encoding_error> {
            if (occurrences.size() == 1u) {
                const query_occurrence_reference occurrence =
                    query_occurrence_reference_(occurrences.front(), packing_layout);
                if (emission_context.scratch == nullptr || emission_context.batch == nullptr ||
                    occurrence.batch_query_ordinal >= emission_context.queries.size() ||
                    occurrence.query_ordinal >=
                        emission_context.queries[occurrence.batch_query_ordinal]
                            .minimizers.size()) {
                    return minimizer_generation_encoding_error::partition_id;
                }

                const std::size_t query_ordinal = occurrence.batch_query_ordinal;
                const auto position_strand =
                    query_position_strand_(occurrence, emission_context.queries);
                if (!position_strand) {
                    return position_strand.error();
                }
                if (emission_context.layout_base >
                        emission_context.scratch->block_query_layouts_.size() ||
                    query_ordinal >= emission_context.scratch->block_query_layouts_.size() -
                                         emission_context.layout_base ||
                    query_ordinal >= emission_context.batch->query_count_) {
                    return minimizer_generation_encoding_error::arithmetic_overflow;
                }
                const fragment_seed_query& query = emission_context.queries[query_ordinal];
                prepared_query_anchors& output = emission_context.batch->queries_[query_ordinal];
                block_query_anchor_layout& layout =
                    emission_context.scratch
                        ->block_query_layouts_[emission_context.layout_base + query_ordinal];

                std::uint64_t selection_postings = 0u;
                for (std::uint8_t span_ordinal = 0u; span_ordinal < selected.span_count;
                     ++span_ordinal) {
                    const compact_hash32_posting_span_view& span = selected.spans[span_ordinal];

                    // Target strand is invariant for one authoritative
                    // span. Build the query-dependent anchor fields once
                    // at reference position zero; the typed posting loop
                    // below supplies only the changing reference position.
                    const auto prepared_anchor =
                        make_fragment_anchor_(static_cast<std::uint64_t>(span.target_strand_one),
                                              *position_strand,
                                              query.query_length,
                                              static_cast<std::uint8_t>(query.kmer_size));
                    if (!prepared_anchor)
                        return prepared_anchor.error();
                    const bioseq::chaining::anchor anchor_template = *prepared_anchor;

                    std::uint64_t* const cursor = anchor_template.is_reverse != 0u
                                                      ? &layout.reverse_cursor
                                                      : &layout.forward_cursor;
                    const std::uint64_t slice_begin = anchor_template.is_reverse != 0u
                                                          ? layout.reverse_begin
                                                          : layout.forward_begin;
                    const std::uint64_t slice_count = anchor_template.is_reverse != 0u
                                                          ? layout.reverse_count
                                                          : layout.forward_count;
                    if (slice_begin > (std::numeric_limits<std::uint64_t>::max)() - slice_count) {
                        return minimizer_generation_encoding_error::arithmetic_overflow;
                    }
                    const std::uint64_t slice_end = slice_begin + slice_count;
                    if (*cursor < slice_begin || *cursor > slice_end ||
                        span.count > slice_end - *cursor || *cursor > output.anchor_count ||
                        span.count > output.anchor_count - *cursor || output.anchors == nullptr) {
                        return minimizer_generation_encoding_error::arithmetic_overflow;
                    }

                    const std::uint64_t initial_cursor = *cursor;
                    std::uint64_t emitted = 0u;
                    bioseq::chaining::anchor* destination =
                        output.anchors.get() + static_cast<std::size_t>(initial_cursor);
                    bool have_previous_target_bp = false;
                    std::uint64_t previous_target_bp = 0u;
                    std::optional<minimizer_generation_encoding_error> decode_visitor_error{};
                    const auto decoded_positions = span.visit_encoded_positions(
                        [&anchor_template,
                         &decode_visitor_error,
                         &destination,
                         &emission_context,
                         &emitted,
                         &have_previous_target_bp,
                         &previous_target_bp](const std::uint64_t encoded_position) noexcept {
                            const std::uint64_t target_bp = encoded_position >> 1u;
                            if (have_previous_target_bp && target_bp < previous_target_bp) {
                                decode_visitor_error =
                                    minimizer_generation_encoding_error::position_range;
                                return false;
                            }
                            previous_target_bp = target_bp;
                            have_previous_target_bp = true;
                            if (target_bp >= emission_context.batch->target_length_) {
                                decode_visitor_error =
                                    minimizer_generation_encoding_error::position_range;
                                return false;
                            }

                            *destination = anchor_template;
                            destination->ref_position = target_bp;
                            ++destination;
                            ++emitted;
                            return true;
                        });

                    // The per-anchor path advances after every
                    // successful store. Commit the same partial progress
                    // before propagating a later decode/visitor error.
                    *cursor = initial_cursor + emitted;
                    if (!decoded_positions) {
                        if (decode_visitor_error) {
                            return decode_visitor_error;
                        }
                        context.direct_error = decoded_positions.error();
                        return minimizer_generation_encoding_error::position_range;
                    }
                    if (decode_visitor_error) {
                        return decode_visitor_error;
                    }
                    if (*decoded_positions != span.count || emitted != span.count) {
                        return minimizer_generation_encoding_error::element_count;
                    }
                    if (span.count > (std::numeric_limits<std::uint64_t>::max)() /
                                         group.occurrence_ref_count ||
                        span.count > (std::numeric_limits<std::uint64_t>::max)() /
                                         span.posting_width_bytes ||
                        !add_checked_(context.anchors_constructed,
                                      span.count * group.occurrence_ref_count) ||
                        !add_checked_(context.posting_bytes_read,
                                      span.count * span.posting_width_bytes) ||
                        !add_checked_(context.postings_visited, span.count) ||
                        !add_checked_(selection_postings, span.count)) {
                        return minimizer_generation_encoding_error::arithmetic_overflow;
                    }
                }
                if (selection_postings != selected.global_count) {
                    return std::optional<minimizer_generation_encoding_error>{
                        minimizer_generation_encoding_error::element_count};
                }
                return std::optional<minimizer_generation_encoding_error>{};
            }

            std::uint64_t selection_postings = 0u;
            for (std::uint8_t span_ordinal = 0u; span_ordinal < selected.span_count;
                 ++span_ordinal) {
                const compact_hash32_posting_span_view& span = selected.spans[span_ordinal];
                bool have_previous_target_bp = false;
                std::uint64_t previous_target_bp = 0u;
                std::optional<minimizer_generation_encoding_error> decode_visitor_error{};
                const auto decoded_positions = span.visit_encoded_positions(
                    [&decode_visitor_error,
                     &have_previous_target_bp,
                     &previous_target_bp,
                     occurrences,
                     &packing_layout,
                     &emission_context](const std::uint64_t encoded_position) noexcept {
                        const std::uint64_t target_bp = encoded_position >> 1u;
                        if (have_previous_target_bp && target_bp < previous_target_bp) {
                            decode_visitor_error =
                                minimizer_generation_encoding_error::position_range;
                            return false;
                        }
                        previous_target_bp = target_bp;
                        have_previous_target_bp = true;

                        for (const auto& occurrence : occurrences) {
                            if (const auto error = emit_compact_direct_anchor_(
                                    encoded_position,
                                    query_occurrence_reference_(occurrence, packing_layout),
                                    emission_context)) {
                                decode_visitor_error = error;
                                return false;
                            }
                        }
                        return true;
                    });
                if (!decoded_positions) {
                    if (decode_visitor_error) {
                        return decode_visitor_error;
                    }
                    context.direct_error = decoded_positions.error();
                    return minimizer_generation_encoding_error::position_range;
                }
                if (decode_visitor_error)
                    return decode_visitor_error;
                if (*decoded_positions != span.count) {
                    return minimizer_generation_encoding_error::element_count;
                }
                if (span.count >
                        (std::numeric_limits<std::uint64_t>::max)() / group.occurrence_ref_count ||
                    span.count >
                        (std::numeric_limits<std::uint64_t>::max)() / span.posting_width_bytes ||
                    !add_checked_(context.anchors_constructed,
                                  span.count * group.occurrence_ref_count) ||
                    !add_checked_(context.posting_bytes_read,
                                  span.count * span.posting_width_bytes) ||
                    !add_checked_(context.postings_visited, span.count) ||
                    !add_checked_(selection_postings, span.count)) {
                    return minimizer_generation_encoding_error::arithmetic_overflow;
                }
            }
            if (selection_postings != selected.global_count) {
                return std::optional<minimizer_generation_encoding_error>{
                    minimizer_generation_encoding_error::element_count};
            }
            return std::optional<minimizer_generation_encoding_error>{};
        });
    return !context.error && !context.direct_error;
}

/**
 * Runs the compact-H32 direct anchor pipeline: unique lookup, layout pass, anchor
 * allocation, emission pass, and per-query local ordering.
 *
 * \throws persisted_minimizer_lookup_error when admission, lookup, layout, emission,
 *     or stats reconciliation fails.
 */
inline void prepared_minimizer_fragment_reader::prepare_compact_direct_anchors_(
    bioseq::parallelism::memory_admission_controller& admission,
    std::span<const fragment_seed_query> queries,
    std::uint32_t runtime_max_occ,
    query_wave_scratch& scratch,
    prepared_fragment_anchor_batch& batch) const {
    using bioseq::common::saturating_add;
    using bioseq::common::saturating_mul;
    if (!compact_direct_) {
        throw persisted_minimizer_lookup_error("compact-H32 direct query contract is inconsistent");
    }
    const std::size_t direct_query_plan_count = scratch.query_plans_.size();

    batch.stats_.compact_direct_waves = 1u;
    batch.stats_.compact_direct_unique_query_groups = direct_query_plan_count;
    const std::uint64_t maximum_plan_bytes = saturating_mul(
        saturating_mul(direct_query_plan_count, sizeof(compact_hash32_unique_selection)), 2u);
    if (maximum_plan_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        throw persisted_minimizer_lookup_error("compact-H32 accepted-plan admission size overflow");
    }
    auto direct_plan_permit = admission.try_reserve(maximum_plan_bytes);
    if (!direct_plan_permit) {
        throw persisted_minimizer_lookup_error(
            persisted_minimizer_lookup_error::reason::admission_or_allocation,
            "compact-H32 accepted-plan admission failed");
    }

    const std::uint64_t lookup_started_ns = stage_now_ns_();
    auto queried = query_compact_hash32_direct_index_unique_projected(
        compact_direct_->view_,
        direct_query_plan_count,
        [&query_plans = scratch.query_plans_](std::size_t ordinal) noexcept -> std::uint32_t {
            return query_plans[ordinal].hash;
        },
        runtime_max_occ);
    if (!queried) {
        if (queried.error() == direct_inverted_index_error::admission ||
            queried.error() == direct_inverted_index_error::allocation) {
            throw persisted_minimizer_lookup_error(
                persisted_minimizer_lookup_error::reason::admission_or_allocation,
                "compact-H32 unique lookup allocation/admission "
                "failed");
        }
        throw persisted_minimizer_lookup_error(
            "compact-H32 unique lookup failed; direct_error=" +
            std::to_string(static_cast<unsigned>(queried.error())));
    }
    compact_hash32_unique_query_plan plan = std::move(*queried);
    const std::uint64_t retained_plan_bytes = plan.retained_selection_capacity_bytes();
    if (plan.stats.selection_peak_capacity_bytes > direct_plan_permit->bytes() ||
        retained_plan_bytes != plan.stats.selection_retained_capacity_bytes) {
        throw persisted_minimizer_lookup_error("compact-H32 accepted-plan peak exceeded admission");
    }
    direct_plan_permit->shrink_to(retained_plan_bytes);
    batch.stats_.compact_direct_lookup_ns = elapsed_stage_ns_(lookup_started_ns);
    batch.stats_.compact_direct_unique_lookups = plan.stats.distinct_query_hash_count;
    batch.stats_.compact_direct_selected_hashes = plan.stats.unique_selected_hash_count;
    batch.stats_.compact_direct_runtime_rejected_hashes = plan.stats.runtime_rejected_hash_count;
    batch.stats_.compact_direct_runtime_rejected_postings =
        plan.stats.runtime_rejected_posting_count;
    batch.stats_.compact_direct_selected_postings = plan.stats.selected_posting_count;
    batch.stats_.compact_direct_rejected_posting_reads = plan.stats.rejected_posting_reads;
    batch.stats_.compact_direct_plan_bytes =
        saturating_mul(plan.selections.size(), sizeof(compact_hash32_unique_selection));
    batch.stats_.compact_direct_plan_reserve_calls = plan.stats.selection_reserve_call_count;
    batch.stats_.compact_direct_plan_reserve_requested_elements =
        plan.stats.selection_reserve_requested_elements;
    batch.stats_.compact_direct_plan_initial_capacity_elements =
        plan.stats.selection_initial_capacity_elements;
    batch.stats_.compact_direct_plan_fill_reallocations =
        plan.stats.selection_fill_reallocation_count;
    batch.stats_.compact_direct_plan_compaction_allocations =
        plan.stats.selection_compaction_allocation_count;
    batch.stats_.compact_direct_plan_storage_allocations =
        plan.stats.selection_storage_allocation_count;
    batch.stats_.compact_direct_plan_retained_capacity_elements =
        plan.stats.selection_retained_capacity_elements;
    batch.stats_.compact_direct_plan_retained_capacity_bytes =
        plan.stats.selection_retained_capacity_bytes;
    batch.stats_.peak_compact_direct_plan_capacity_bytes = plan.stats.selection_peak_capacity_bytes;
    const std::uint64_t expected_storage_allocations =
        direct_query_plan_count == 0u                                                  ? 0u
        : plan.selections.empty() || plan.selections.size() == direct_query_plan_count ? 1u
                                                                                       : 2u;
    const std::uint64_t expected_compaction_allocations =
        !plan.selections.empty() && plan.selections.size() < direct_query_plan_count ? 1u : 0u;
    const std::uint64_t expected_peak_capacity_elements =
        expected_compaction_allocations == 0u
            ? direct_query_plan_count
            : saturating_add(direct_query_plan_count, plan.selections.size());
    const std::uint64_t expected_peak_capacity_bytes =
        saturating_mul(expected_peak_capacity_elements, sizeof(compact_hash32_unique_selection));
    if (plan.stats.query_occurrence_count != direct_query_plan_count ||
        plan.stats.distinct_query_hash_count != direct_query_plan_count ||
        plan.stats.unique_selected_hash_count != plan.selections.size() ||
        plan.stats.rejected_posting_reads != 0u || plan.stats.selection_reserve_call_count != 1u ||
        plan.stats.selection_reserve_requested_elements != direct_query_plan_count ||
        plan.stats.selection_initial_capacity_elements != direct_query_plan_count ||
        plan.stats.selection_fill_reallocation_count != 0u ||
        plan.stats.selection_compaction_allocation_count != expected_compaction_allocations ||
        plan.stats.selection_storage_allocation_count != expected_storage_allocations ||
        plan.stats.selection_retained_capacity_elements != plan.selections.size() ||
        plan.stats.selection_retained_capacity_bytes != batch.stats_.compact_direct_plan_bytes ||
        plan.stats.selection_peak_capacity_bytes != expected_peak_capacity_bytes ||
        retained_plan_bytes != batch.stats_.compact_direct_plan_bytes ||
        batch.stats_.compact_direct_plan_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        expected_peak_capacity_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        throw persisted_minimizer_lookup_error("compact-H32 unique lookup stats disagrees");
    }

    const bool have_selected_postings = plan.stats.selected_posting_count != 0u;
    const std::size_t selection_shard_count =
        have_selected_postings
            ? std::min(plan.selections.size(), compact_direct_selection_shard_target_)
            : 0u;
    if (have_selected_postings &&
        (selection_shard_count == 0u || queries.empty() ||
         selection_shard_count > (std::numeric_limits<std::size_t>::max)() / queries.size())) {
        throw persisted_minimizer_lookup_error(
            "compact-H32 shard/query layout size is inconsistent");
    }
    const std::size_t shard_query_layout_count = selection_shard_count * queries.size();
    batch.stats_.compact_direct_selection_shards = selection_shard_count;
    batch.stats_.compact_direct_shard_query_layouts = shard_query_layout_count;
    const std::uint64_t orientation_started_ns = stage_now_ns_();
    if (have_selected_postings) {
        const std::size_t old_layout_capacity = scratch.block_query_layouts_.capacity();
        scratch.ensure_vector_capacity_(scratch.block_query_layouts_, shard_query_layout_count);
        batch.stats_.block_layout_allocations =
            scratch.block_query_layouts_.capacity() > old_layout_capacity ? 1u : 0u;
        batch.stats_.peak_block_layout_capacity_bytes = saturating_mul(
            scratch.block_query_layouts_.capacity(), sizeof(block_query_anchor_layout));
        scratch.block_query_layouts_.assign(shard_query_layout_count, block_query_anchor_layout{});

        const std::size_t old_block_stats_capacity = scratch.block_stats_.capacity();
        scratch.ensure_vector_capacity_(scratch.block_stats_, queries.size());
        batch.stats_.block_stats_allocations =
            scratch.block_stats_.capacity() > old_block_stats_capacity ? 1u : 0u;
        batch.stats_.peak_block_stats_capacity_bytes =
            saturating_mul(scratch.block_stats_.capacity(), sizeof(fragment_block_wave_stats));
        scratch.block_stats_.assign(queries.size(), fragment_block_wave_stats{});
    }

    const auto selection_access =
        bind_compact_hash32_direct_index_selection_spans(compact_direct_->view_, plan);
    if (!selection_access || selection_access->selection_count() != plan.selections.size()) {
        throw persisted_minimizer_lookup_error(
            "compact-H32 selection-range binding failed; direct_error=" +
            std::to_string(static_cast<unsigned>(selection_access
                                                     ? direct_inverted_index_error::query_plan
                                                     : selection_access.error())));
    }

    std::array<compact_direct_layout_pass_context, compact_direct_selection_shard_target_>
        layout_contexts{};
    for (std::size_t shard_ordinal = 0u; shard_ordinal < selection_shard_count; ++shard_ordinal) {
        layout_contexts[shard_ordinal] = compact_direct_layout_pass_context{
            .reader = this,
            .plan = &plan,
            .queries = queries,
            .scratch = &scratch,
            .shard_ordinal = shard_ordinal,
        };
    }
    if (have_selected_postings) {
        parallel_entries_(
            selection_shard_count,
            [&selection_access,
             &layout_contexts,
             selection_count = plan.selections.size(),
             selection_shard_count](std::size_t shard_begin, std::size_t shard_end) noexcept {
                for (std::size_t shard_ordinal = shard_begin; shard_ordinal < shard_end;
                     ++shard_ordinal) {
                    compact_direct_layout_pass_context& context = layout_contexts[shard_ordinal];
                    const compact_direct_selection_shard_range range =
                        compact_direct_selection_shard_range_(
                            selection_count, selection_shard_count, shard_ordinal);
                    auto visited = visit_compact_hash32_direct_index_selection_span_range(
                        *selection_access,
                        range.begin,
                        range.end,
                        [&context](
                            const compact_hash32_selection_posting_spans& selected) noexcept {
                            return visit_compact_direct_layout_selection_(context, selected);
                        });
                    flush_compact_direct_selected_page_advice_(context);
                    if (visited) {
                        context.visit_stats = *visited;
                    } else if (!context.error && !context.direct_error) {
                        context.direct_error = visited.error();
                    }
                }
            });
    }

    // Every shard binds authenticated descriptors and span geometry, but
    // deliberately does not load one encoded posting position.
    batch.stats_.compact_direct_layout_postings_visited = 0u;
    std::uint64_t layout_logical_postings = 0u;
    std::size_t layout_selection_end = 0u;
    for (std::size_t shard_ordinal = 0u; shard_ordinal < selection_shard_count; ++shard_ordinal) {
        const compact_direct_layout_pass_context& context = layout_contexts[shard_ordinal];
        if (context.error) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 descriptor layout shard failed; wire_error=" +
                std::to_string(static_cast<unsigned>(*context.error)));
        }
        if (context.direct_error || !context.visit_stats) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 descriptor layout shard failed; direct_error=" +
                std::to_string(static_cast<unsigned>(
                    context.direct_error ? *context.direct_error
                                         : direct_inverted_index_error::query_plan)));
        }
        const compact_direct_selection_shard_range range = compact_direct_selection_shard_range_(
            plan.selections.size(), selection_shard_count, shard_ordinal);
        const compact_hash32_selection_span_visit_stats& visit = *context.visit_stats;
        if (range.begin != layout_selection_end || range.end <= range.begin ||
            range.end > plan.selections.size() ||
            visit.selection_callbacks != range.end - range.begin ||
            visit.descriptors_examined != range.end - range.begin ||
            visit.postings_exposed != context.logical_postings_counted ||
            visit.rank_checkpoints_examined != 0u || visit.membership_words_examined != 0u ||
            !add_checked_(layout_logical_postings, context.logical_postings_counted) ||
            !add_checked_(batch.stats_.compact_direct_layout_selection_callbacks,
                          visit.selection_callbacks) ||
            !add_checked_(batch.stats_.compact_direct_layout_spans_exposed, visit.spans_exposed) ||
            !add_checked_(batch.stats_.compact_direct_layout_descriptors_examined,
                          visit.descriptors_examined) ||
            !add_checked_(batch.stats_.compact_direct_traversal_rank_checkpoints_examined,
                          visit.rank_checkpoints_examined) ||
            !add_checked_(batch.stats_.compact_direct_traversal_membership_words_examined,
                          visit.membership_words_examined)) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 descriptor layout shard receipt disagrees");
        }
        layout_selection_end = range.end;
        ++batch.stats_.compact_direct_layout_shards_completed;
    }
    if (layout_logical_postings != plan.stats.selected_posting_count ||
        layout_selection_end != plan.selections.size() ||
        batch.stats_.compact_direct_layout_selection_callbacks != plan.selections.size() ||
        batch.stats_.compact_direct_layout_descriptors_examined != plan.selections.size()) {
        throw persisted_minimizer_lookup_error(
            "compact-H32 descriptor layout aggregate receipt disagrees");
    }

    for (std::size_t query_ordinal = 0u; query_ordinal < queries.size(); ++query_ordinal) {
        if (!have_selected_postings) {
            batch.queries_[query_ordinal].anchor_count = 0u;
            continue;
        }
        std::uint64_t forward_cursor = 0u;
        for (std::size_t shard_ordinal = 0u; shard_ordinal < selection_shard_count;
             ++shard_ordinal) {
            block_query_anchor_layout& layout =
                scratch.block_query_layouts_[shard_ordinal * queries.size() + query_ordinal];
            layout.forward_begin = forward_cursor;
            layout.forward_cursor = forward_cursor;
            if (!add_checked_(forward_cursor, layout.forward_count)) {
                throw persisted_minimizer_lookup_error("compact-H32 forward shard prefix overflow");
            }
        }
        std::uint64_t reverse_cursor = forward_cursor;
        for (std::size_t shard_ordinal = 0u; shard_ordinal < selection_shard_count;
             ++shard_ordinal) {
            block_query_anchor_layout& layout =
                scratch.block_query_layouts_[shard_ordinal * queries.size() + query_ordinal];
            layout.reverse_begin = reverse_cursor;
            layout.reverse_cursor = reverse_cursor;
            if (!add_checked_(reverse_cursor, layout.reverse_count)) {
                throw persisted_minimizer_lookup_error("compact-H32 reverse shard prefix overflow");
            }
        }
        batch.queries_[query_ordinal].anchor_count = reverse_cursor;
    }

    std::uint64_t final_owned_bytes = batch.owned_bytes_;
    for (std::size_t query_ordinal = 0u; query_ordinal < queries.size(); ++query_ordinal) {
        const prepared_query_anchors& query = batch.queries_[query_ordinal];
        const std::uint64_t anchor_bytes =
            saturating_mul(query.anchor_count, sizeof(bioseq::chaining::anchor));
        final_owned_bytes = saturating_add(final_owned_bytes, anchor_bytes);
        if (anchor_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            final_owned_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            query.anchor_count >
                (std::numeric_limits<std::size_t>::max)() / sizeof(bioseq::chaining::anchor)) {
            throw persisted_minimizer_lookup_error("compact-H32 exact anchor size overflow");
        }
    }
    if (!batch.permit_.grow_to(final_owned_bytes)) {
        throw persisted_minimizer_lookup_error(
            persisted_minimizer_lookup_error::reason::admission_or_allocation,
            "compact-H32 exact anchor admission failed");
    }
    try {
        for (std::size_t query_ordinal = 0u; query_ordinal < queries.size(); ++query_ordinal) {
            prepared_query_anchors& query = batch.queries_[query_ordinal];
            if (query.anchor_count == 0u)
                continue;
            query.anchors = std::make_unique_for_overwrite<bioseq::chaining::anchor[]>(
                static_cast<std::size_t>(query.anchor_count));
        }
    } catch (const std::bad_alloc&) {
        throw persisted_minimizer_lookup_error(
            persisted_minimizer_lookup_error::reason::admission_or_allocation,
            "compact-H32 exact anchor allocation failed");
    }
    batch.owned_bytes_ = final_owned_bytes;
    batch.permit_.shrink_to(batch.owned_bytes_);
    batch.stats_.orientation_layout_ns = elapsed_stage_ns_(orientation_started_ns);

    const std::uint64_t emission_started_ns = stage_now_ns_();
    std::array<compact_direct_emission_pass_context, compact_direct_selection_shard_target_>
        emission_contexts{};
    for (std::size_t shard_ordinal = 0u; shard_ordinal < selection_shard_count; ++shard_ordinal) {
        emission_contexts[shard_ordinal] = compact_direct_emission_pass_context{
            .reader = this,
            .plan = &plan,
            .queries = queries,
            .scratch = &scratch,
            .batch = &batch,
            .shard_ordinal = shard_ordinal,
        };
    }
    if (have_selected_postings) {
        parallel_entries_(
            selection_shard_count,
            [&selection_access,
             &emission_contexts,
             selection_count = plan.selections.size(),
             selection_shard_count](std::size_t shard_begin, std::size_t shard_end) noexcept {
                for (std::size_t shard_ordinal = shard_begin; shard_ordinal < shard_end;
                     ++shard_ordinal) {
                    compact_direct_emission_pass_context& context =
                        emission_contexts[shard_ordinal];
                    const compact_direct_selection_shard_range range =
                        compact_direct_selection_shard_range_(
                            selection_count, selection_shard_count, shard_ordinal);
                    auto visited = visit_compact_hash32_direct_index_selection_span_range(
                        *selection_access,
                        range.begin,
                        range.end,
                        [&context](
                            const compact_hash32_selection_posting_spans& selected) noexcept {
                            return visit_compact_direct_emission_selection_(context, selected);
                        });
                    if (visited) {
                        context.visit_stats = *visited;
                    } else if (!context.error && !context.direct_error) {
                        context.direct_error = visited.error();
                    }
                }
            });
    }
    batch.stats_.compact_direct_posting_visit_passes = have_selected_postings ? 1u : 0u;
    std::uint64_t emission_posting_bytes = 0u;
    std::uint64_t emission_postings = 0u;
    std::uint64_t emission_anchors = 0u;
    std::size_t emission_selection_end = 0u;
    for (std::size_t shard_ordinal = 0u; shard_ordinal < selection_shard_count; ++shard_ordinal) {
        const compact_direct_emission_pass_context& context = emission_contexts[shard_ordinal];
        if (context.error) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 emission shard failed; wire_error=" +
                std::to_string(static_cast<unsigned>(*context.error)));
        }
        if (context.direct_error || !context.visit_stats) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 emission shard failed; direct_error=" +
                std::to_string(static_cast<unsigned>(
                    context.direct_error ? *context.direct_error
                                         : direct_inverted_index_error::query_plan)));
        }
        const compact_direct_selection_shard_range range = compact_direct_selection_shard_range_(
            plan.selections.size(), selection_shard_count, shard_ordinal);
        const compact_hash32_selection_span_visit_stats& visit = *context.visit_stats;
        if (range.begin != emission_selection_end || range.end <= range.begin ||
            range.end > plan.selections.size() ||
            visit.selection_callbacks != range.end - range.begin ||
            visit.descriptors_examined != range.end - range.begin ||
            visit.postings_exposed != context.postings_visited ||
            visit.rank_checkpoints_examined != 0u || visit.membership_words_examined != 0u ||
            !add_checked_(emission_posting_bytes, context.posting_bytes_read) ||
            !add_checked_(emission_postings, context.postings_visited) ||
            !add_checked_(emission_anchors, context.anchors_constructed) ||
            !add_checked_(batch.stats_.compact_direct_emission_selection_callbacks,
                          visit.selection_callbacks) ||
            !add_checked_(batch.stats_.compact_direct_emission_spans_exposed,
                          visit.spans_exposed) ||
            !add_checked_(batch.stats_.compact_direct_emission_descriptors_examined,
                          visit.descriptors_examined) ||
            !add_checked_(batch.stats_.compact_direct_traversal_rank_checkpoints_examined,
                          visit.rank_checkpoints_examined) ||
            !add_checked_(batch.stats_.compact_direct_traversal_membership_words_examined,
                          visit.membership_words_examined)) {
            throw persisted_minimizer_lookup_error("compact-H32 emission shard receipt disagrees");
        }
        emission_selection_end = range.end;
        ++batch.stats_.compact_direct_emission_shards_completed;
    }
    batch.stats_.compact_direct_emission_postings_visited = emission_postings;
    batch.stats_.direct_emission_ns = elapsed_stage_ns_(emission_started_ns);
    if (emission_postings != plan.stats.selected_posting_count ||
        emission_selection_end != plan.selections.size() ||
        batch.stats_.compact_direct_emission_selection_callbacks != plan.selections.size() ||
        batch.stats_.compact_direct_emission_descriptors_examined != plan.selections.size() ||
        batch.stats_.compact_direct_emission_spans_exposed !=
            batch.stats_.compact_direct_layout_spans_exposed) {
        throw persisted_minimizer_lookup_error("compact-H32 emission aggregate receipt disagrees");
    }

    if (have_selected_postings) {
        for (std::size_t shard_ordinal = 0u; shard_ordinal < selection_shard_count;
             ++shard_ordinal) {
            for (std::size_t query_ordinal = 0u; query_ordinal < queries.size(); ++query_ordinal) {
                const block_query_anchor_layout& layout =
                    scratch.block_query_layouts_[shard_ordinal * queries.size() + query_ordinal];
                if (layout.forward_begin >
                        (std::numeric_limits<std::uint64_t>::max)() - layout.forward_count ||
                    layout.reverse_begin >
                        (std::numeric_limits<std::uint64_t>::max)() - layout.reverse_count ||
                    layout.forward_cursor != layout.forward_begin + layout.forward_count ||
                    layout.reverse_cursor != layout.reverse_begin + layout.reverse_count) {
                    throw persisted_minimizer_lookup_error(
                        "compact-H32 shard emission cursor receipt "
                        "disagrees");
                }
            }
        }
    }

    std::atomic<std::uint32_t> first_error{0u};
    const auto run_local_order_stage = [this, queries, &batch, &scratch, &first_error](
                                           bool normalize_ties) {
        first_error.store(0u, std::memory_order_relaxed);
        parallel_entries_(
            queries.size(),
            [queries,
             &batch,
             &scratch,
             &first_error,
             normalize_ties,
             authenticated_target_length =
                 manifest_.target_length](std::size_t query_begin, std::size_t query_end) noexcept {
                for (std::size_t query_ordinal = query_begin; query_ordinal < query_end;
                     ++query_ordinal) {
                    if (first_error.load(std::memory_order_relaxed) != 0u ||
                        query_ordinal >= scratch.block_stats_.size()) {
                        if (query_ordinal >= scratch.block_stats_.size()) {
                            record_error_(first_error,
                                          minimizer_generation_encoding_error::arithmetic_overflow);
                        }
                        return;
                    }
                    std::uint64_t query_order_bytes = 0u;
                    const block_query_anchor_layout& first_shard_layout =
                        scratch.block_query_layouts_[query_ordinal];
                    auto& query = batch.queries_[query_ordinal];
                    const std::uint64_t forward_count = first_shard_layout.reverse_begin;
                    if (forward_count > query.anchor_count) {
                        record_error_(first_error,
                                      minimizer_generation_encoding_error::arithmetic_overflow);
                        return;
                    }
                    const std::uint64_t reverse_count = query.anchor_count - forward_count;
                    const auto order_slice =
                        [normalize_ties, &query_order_bytes, authenticated_target_length](
                            bioseq::chaining::anchor* anchors, std::uint64_t count) noexcept {
                            if (count < 2u)
                                return true;
                            const auto stats =
                                normalize_ties
                                    ? normalize_anchor_slice_ties_(anchors,
                                                                   static_cast<std::size_t>(count))
                                    : sort_anchor_slice_primary_(anchors,
                                                                 static_cast<std::size_t>(count),
                                                                 authenticated_target_length);
                            const std::uint64_t bytes =
                                normalize_ties ? stats.tie_bytes : stats.primary_bytes;
                            return !stats.saturated && add_checked_(query_order_bytes, bytes);
                        };
                    if ((forward_count > 1u && !order_slice(query.anchors.get(), forward_count)) ||
                        (reverse_count > 1u &&
                         !order_slice(query.anchors.get() + static_cast<std::size_t>(forward_count),
                                      reverse_count))) {
                        record_error_(first_error,
                                      minimizer_generation_encoding_error::arithmetic_overflow);
                        return;
                    }
                    fragment_block_wave_stats& block_stats = scratch.block_stats_[query_ordinal];
                    if (normalize_ties) {
                        block_stats.tie_normalization_bytes = query_order_bytes;
                    } else {
                        block_stats.local_sort_bytes = query_order_bytes;
                    }
                }
            });
        if (const auto error = recorded_error_(first_error)) {
            throw persisted_minimizer_lookup_error(
                std::string{normalize_ties ? "compact-H32 tie normalization failed; "
                                           : "compact-H32 local primary sort failed; "} +
                "wire_error=" + std::to_string(static_cast<unsigned>(*error)));
        }
    };

    const std::uint64_t local_sort_started_ns = stage_now_ns_();
    if (have_selected_postings)
        run_local_order_stage(false);
    batch.stats_.local_sort_ns = elapsed_stage_ns_(local_sort_started_ns);
    const std::uint64_t tie_started_ns = stage_now_ns_();
    if (have_selected_postings)
        run_local_order_stage(true);
    batch.stats_.tie_normalization_ns = elapsed_stage_ns_(tie_started_ns);

    std::uint64_t expected_constructed = 0u;
    for (std::size_t query_ordinal = 0u; query_ordinal < queries.size(); ++query_ordinal) {
        if (!add_checked_(expected_constructed, batch.queries_[query_ordinal].anchor_count)) {
            throw persisted_minimizer_lookup_error("compact-H32 anchor stats overflow");
        }
    }
    for (const fragment_block_wave_stats& block_stats : scratch.block_stats_) {
        if (!add_checked_(batch.stats_.local_sort_bytes, block_stats.local_sort_bytes) ||
            !add_checked_(batch.stats_.tie_normalization_bytes,
                          block_stats.tie_normalization_bytes)) {
            throw persisted_minimizer_lookup_error("compact-H32 stats reduction overflow");
        }
    }
    batch.stats_.pass2_position_bytes = 0u;
    batch.stats_.pass3_position_bytes = emission_posting_bytes;
    batch.stats_.anchors_constructed = emission_anchors;
    const std::uint64_t expected_posting_bytes =
        saturating_mul(plan.stats.selected_posting_count, plan.posting_width_bytes);
    const std::uint64_t exact_final_bytes =
        saturating_mul(expected_constructed, sizeof(bioseq::chaining::anchor));
    if (batch.stats_.compact_direct_unique_lookups !=
            batch.stats_.compact_direct_unique_query_groups ||
        batch.stats_.compact_direct_selection_shards != selection_shard_count ||
        batch.stats_.compact_direct_shard_query_layouts != shard_query_layout_count ||
        batch.stats_.compact_direct_layout_shards_completed != selection_shard_count ||
        batch.stats_.compact_direct_emission_shards_completed != selection_shard_count ||
        batch.stats_.compact_direct_posting_visit_passes != (have_selected_postings ? 1u : 0u) ||
        batch.stats_.compact_direct_layout_postings_visited != 0u ||
        batch.stats_.compact_direct_emission_postings_visited !=
            plan.stats.selected_posting_count ||
        batch.stats_.compact_direct_layout_selection_callbacks != plan.selections.size() ||
        batch.stats_.compact_direct_emission_selection_callbacks != plan.selections.size() ||
        (plan.selections.empty() ? batch.stats_.compact_direct_layout_spans_exposed != 0u
                                 : batch.stats_.compact_direct_layout_spans_exposed == 0u) ||
        batch.stats_.compact_direct_layout_spans_exposed !=
            batch.stats_.compact_direct_emission_spans_exposed ||
        batch.stats_.compact_direct_layout_descriptors_examined != plan.selections.size() ||
        batch.stats_.compact_direct_emission_descriptors_examined != plan.selections.size() ||
        batch.stats_.compact_direct_traversal_rank_checkpoints_examined != 0u ||
        batch.stats_.compact_direct_traversal_membership_words_examined != 0u ||
        batch.stats_.compact_direct_target_control_scan_bytes != 0u ||
        batch.stats_.compact_direct_target_control_scan_passes != 0u ||
        batch.stats_.compact_direct_fallbacks != 0u ||
        expected_posting_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        exact_final_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        batch.stats_.pass2_position_bytes != 0u ||
        batch.stats_.pass3_position_bytes != expected_posting_bytes ||
        batch.stats_.anchors_constructed != expected_constructed ||
        emission_anchors != expected_constructed) {
        throw persisted_minimizer_lookup_error("compact-H32 direct-route stats equation failed");
    }
    batch.stats_.exact_final_bytes = exact_final_bytes;
    batch.stats_.peak_exact_final_bytes = exact_final_bytes;
}

} // namespace bioseq::indexing
