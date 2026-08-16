/**
 * @file compact_direct/reader/anchor_planning.cppm
 * @brief Query-wave planning, admission, grouped execution, and final anchors.
 */

module;

#include <libassert/assert.hpp>

export module bioseq.indexing.minimizer_hashmap:compact_direct.reader_anchor_planning;

import :compact_direct.reader_contract;
import :compact_direct.reader_anchor_occurrences;
import :compact_direct.reader_codec;
import :compact_direct.reader_direct_execution;
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

template <bool p_oracle, bool p_compact_direct>
[[nodiscard]] inline prepared_fragment_anchor_batch
prepared_minimizer_fragment_reader::prepare_fragment_anchor_batch_impl_(
    bioseq::parallelism::memory_admission_controller& admission,
    std::span<const fragment_seed_query> queries,
    std::int32_t max_occ,
    query_wave_scratch& scratch,
    bool verify_compositional_order_for_testing) const {
    using query_hash_plan = query_wave_scratch::query_hash_plan;
    using bioseq::common::saturating_add;
    using bioseq::common::saturating_mul;

    if (!ready_) {
        throw persisted_minimizer_lookup_error("fragment-native grouped-SoA reader is not ready");
    }
    if constexpr (p_compact_direct) {
        if (!compact_direct_) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 direct route was explicitly selected "
                "without an attached direct index");
        }
        if (backing_kind_ == backing_kind::compact_direct_only) {
            if (mapped_payload_owner_ != nullptr || !mapped_payload_.empty() ||
                directory_.owned_capacity_bytes() != 0u || entry_count_ != 0u ||
                entry_plans_ != nullptr || block_plans_ != nullptr || block_count_ != 0u ||
                owned_bytes_ != 0u || permit_.bytes() != 0u) {
                throw persisted_minimizer_lookup_error(
                    "compact-direct-only reader retained forbidden "
                    "grouped-SoA state");
            }
        } else if (mapped_payload_owner_ == nullptr ||
                   mapped_payload_.size() != manifest_.payload_bytes) {
            throw persisted_minimizer_lookup_error(
                "fragment-native grouped-SoA reader is not ready");
        }
    } else if (backing_kind_ != backing_kind::grouped_soa_fragments ||
               mapped_payload_owner_ == nullptr ||
               mapped_payload_.size() != manifest_.payload_bytes) {
        throw persisted_minimizer_lookup_error("fragment-native grouped-SoA reader is not ready");
    }
    if (max_occ < 0 || static_cast<std::uint64_t>(max_occ) != max_occ_ ||
        queries.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        throw persisted_minimizer_lookup_error(
            "fragment-native grouped-SoA query policy is inconsistent");
    }
    // Freeze the manifest-selected wire route once for the complete wave.
    // Offset width remains an entry-local dispatch inside the fixed leaf family.
    [[maybe_unused]] const bool use_fixed_h32_c16_p16 =
        wire_dispatch_ == fragment_encoding_dispatch::h32_c16_p16;
    static_assert(!(p_oracle && p_compact_direct));
    constexpr bool use_bucket_direct_matcher = !p_oracle && !p_compact_direct;

    // Acquire the reusable scratch before reading or mutating any of its
    // non-atomic owners. The busy state remains published through the
    // synchronous joins that borrow the prepared batch and is cleared by
    // complete_wave_after_join. A failed preparation clears it in the
    // catch path below, so there is no reentrant vector/permit race.
    query_wave_scratch::wave_state expected_state = query_wave_scratch::wave_state::idle;
    if (!scratch.wave_state_.compare_exchange_strong(expected_state,
                                                     query_wave_scratch::wave_state::preparing,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
        throw persisted_minimizer_lookup_error("fragment-native query scratch is already in use");
    }

    try {
        if (!scratch.permit_ || scratch.admission_ != &admission) {
            throw persisted_minimizer_lookup_error(
                "fragment-native grouped-SoA query scratch identity is inconsistent");
        }
        scratch.clear_wave_storage_noexcept_();
        if constexpr (p_compact_direct) {
            // This route never probes grouped-SoA controls.  Retained Bloom,
            // radix, group-directory, and atomic-counter owners from a
            // preceding control-scan wave are released before direct planning
            // so they cannot overlap the accepted-only direct plan.
            {
                std::vector<std::uint32_t> no_group_offsets;
                std::vector<std::uint32_t> no_radix;
                scratch.query_group_offsets_.swap(no_group_offsets);
                scratch.query_plan_radix_.swap(no_radix);
            }
            scratch.query_membership_filter_.reset();
            scratch.query_membership_filter_word_capacity_ = 0u;
            scratch.occurrence_counts_.reset();
            scratch.occurrence_count_capacity_ = 0u;
            scratch.active_occurrence_count_ = 0u;
            scratch.permit_.shrink_to(scratch.retained_capacity_bytes_());
        }
        if (use_bucket_direct_matcher && scratch.query_plans_.capacity() != 0u) {
            {
                std::vector<query_hash_plan> no_baseline_plans;
                scratch.query_plans_.swap(no_baseline_plans);
            }
            scratch.query_plan_reserve_request_ = 0u;
            scratch.permit_.shrink_to(scratch.retained_capacity_bytes_());
        }
        std::uint64_t total_query_minimizers = 0u;
        std::uint64_t largest_query_minimizer_count = 0u;
        for (const fragment_seed_query& query : queries) {
            if (!bioseq::minimizers::is_minimizer_query_length_representable<minimap_sketch_data>(
                    query.query_length) ||
                query.kmer_size <= 0 ||
                query.kmer_size > (std::numeric_limits<std::uint8_t>::max)() ||
                query.kmer_size != static_cast<std::int32_t>(profile_.kmer_size) ||
                query.minimizers.size() > (std::numeric_limits<std::uint32_t>::max)() ||
                total_query_minimizers >
                    (std::numeric_limits<std::uint32_t>::max)() - query.minimizers.size() ||
                (!query.minimizers.empty() &&
                 query.query_length < static_cast<std::uint64_t>(query.kmer_size))) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native grouped-SoA query descriptor is invalid");
            }
            for (const minimap_sketch_data& minimizer : query.minimizers) {
                const std::uint64_t forward_last = minimizer.pos_strand_encoded() >> 1u;
                if (minimizer.kmer() > profile_.max_hash ||
                    minimizer.kmer() > (std::numeric_limits<std::uint32_t>::max)() ||
                    forward_last >= query.query_length ||
                    forward_last + 1u < static_cast<std::uint64_t>(query.kmer_size)) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native grouped-SoA query minimizer is invalid");
                }
            }
            total_query_minimizers += query.minimizers.size();
            largest_query_minimizer_count =
                std::max<std::uint64_t>(largest_query_minimizer_count, query.minimizers.size());
        }
        const auto occurrence_layout = plan_query_occurrence_packing(
            queries.size(), total_query_minimizers, largest_query_minimizer_count);
        if (!occurrence_layout) {
            throw persisted_minimizer_lookup_error(
                "fragment-native query occurrence layout is invalid");
        }
        scratch.occurrence_layout_ = *occurrence_layout;

        auto batch_permit = admission.try_reserve(0u);
        if (!batch_permit) {
            throw persisted_minimizer_lookup_error("fragment-native anchor batch admission failed");
        }
        prepared_fragment_anchor_batch batch{std::move(*batch_permit)};
        batch.query_count_ = queries.size();
        batch.target_length_ = manifest_.target_length;
        batch.max_occ_ = static_cast<std::uint64_t>(max_occ);
        batch.stats_.waves = 1u;
        batch.stats_.queries = queries.size();
        batch.stats_.query_occurrences = total_query_minimizers;
        batch.stats_.packed_occurrence_waves = occurrence_layout->packed() ? 1u : 0u;
        batch.stats_.wide_occurrence_waves = occurrence_layout->packed() ? 0u : 1u;
        batch.stats_.global_sort_calls_skipped = queries.size();

        const std::uint64_t metadata_bytes =
            saturating_mul(queries.size(), sizeof(prepared_query_anchors));
        if (queries.size() >
                (std::numeric_limits<std::size_t>::max)() / sizeof(prepared_query_anchors) ||
            metadata_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            throw persisted_minimizer_lookup_error("fragment-native anchor metadata size overflow");
        }
        if (!batch.permit_.grow_to(metadata_bytes)) {
            throw persisted_minimizer_lookup_error(
                persisted_minimizer_lookup_error::reason::admission_or_allocation,
                "fragment-native anchor metadata admission failed");
        }
        try {
            if (!queries.empty()) {
                batch.queries_ = std::make_unique<prepared_query_anchors[]>(queries.size());
            }
        } catch (const std::bad_alloc&) {
            batch.permit_.shrink_to(0u);
            throw persisted_minimizer_lookup_error(
                persisted_minimizer_lookup_error::reason::admission_or_allocation,
                "fragment-native anchor metadata allocation failed");
        }
        batch.owned_bytes_ = metadata_bytes;
        batch.permit_.shrink_to(batch.owned_bytes_);
        for (std::size_t query_ordinal = 0u; query_ordinal < queries.size(); ++query_ordinal) {
            prepared_query_anchors& destination = batch.queries_[query_ordinal];
            const fragment_seed_query& source = queries[query_ordinal];
            destination.minimizer_data = source.minimizers.data();
            destination.minimizer_count = source.minimizers.size();
            destination.query_length = source.query_length;
            destination.kmer_size = source.kmer_size;
        }

        if constexpr (p_compact_direct) {
            // Ordinary seed lookup treats max_occ==0 as selecting no
            // postings.  Preserve that policy without calling the compact
            // codec's positive-cap query API and without touching either
            // grouped-SoA payload or direct postings.
            if (max_occ == 0) {
                batch.stats_.compact_direct_waves = 1u;
                batch.stats_.compositional_sortedness_receipts = queries.size();
                batch.stats_.exact_final_bytes = 0u;
                batch.stats_.peak_exact_final_bytes = 0u;
                for (std::size_t query_ordinal = 0u; query_ordinal < queries.size();
                     ++query_ordinal) {
                    batch.queries_[query_ordinal].anchor_count = 0u;
                    batch.queries_[query_ordinal].sortedness_receipt =
                        prepared_fragment_anchor_batch::sortedness_receipt_;
                }
                if (!batch.stats_.forbidden_buffers_are_zero() ||
                    batch.owned_bytes_ > batch.permit_.bytes()) {
                    throw persisted_minimizer_lookup_error(
                        "compact-H32 zero-cap ownership stats failed");
                }
                batch.permit_.shrink_to(batch.owned_bytes_);
                batch.ready_ = true;
                scratch.wave_state_.store(query_wave_scratch::wave_state::outstanding,
                                          std::memory_order_release);
                return batch;
            }
        }

        const std::size_t occurrence_count =
            static_cast<std::size_t>(occurrence_layout->occurrence_count);
        prepare_query_occurrences_<p_oracle, p_compact_direct>(
            queries, occurrence_layout, occurrence_count, scratch, batch);

        const std::uint64_t unique_plan_started_ns = stage_now_ns_();
        const std::size_t old_query_plan_capacity = scratch.query_plans_.capacity();
        std::uint64_t compact_direct_group_key_set_checksum =
            compact_direct_query_set_checksum_prefix(
                static_cast<std::uint32_t>(profile_.kmer_size),
                static_cast<std::uint32_t>(manifest_.window_size),
                static_cast<std::uint32_t>(max_occ));
        const auto build_query_plans = [&scratch, &compact_direct_group_key_set_checksum](
                                           const auto& occurrences) {
            std::size_t unique_count = 0u;
            for (std::size_t begin = 0u; begin < occurrences.size();) {
                std::size_t end = begin + 1u;
                while (end < occurrences.size() &&
                       occurrences[end].hash == occurrences[begin].hash) {
                    ++end;
                }
                ++unique_count;
                begin = end;
            }
            if (unique_count > (std::numeric_limits<std::uint32_t>::max)()) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native unique query plan count overflow");
            }
            scratch.query_plan_reserve_request_ = unique_count;
            scratch.ensure_vector_capacity_(scratch.query_plans_, unique_count);
            scratch.query_plans_.clear();
            for (std::size_t begin = 0u; begin < occurrences.size();) {
                std::size_t end = begin + 1u;
                while (end < occurrences.size() &&
                       occurrences[end].hash == occurrences[begin].hash) {
                    ++end;
                }
                const std::uint32_t hash = occurrences[begin].hash;
                scratch.query_plans_.push_back(query_hash_plan{
                    .hash = hash,
                    .occurrence_ref_begin = static_cast<std::uint32_t>(begin),
                    .occurrence_ref_count = static_cast<std::uint32_t>(end - begin),
                    .global_occurrence_count = 0u,
                });
                if constexpr (p_compact_direct) {
                    compact_direct_group_key_set_checksum = compact_direct_query_set_fnv_mix_u64(
                        compact_direct_group_key_set_checksum, hash);
                }
                begin = end;
            }
        };
        const auto count_unique_occurrence_groups = [](const auto& occurrences) noexcept {
            std::size_t unique_count = 0u;
            for (std::size_t begin = 0u; begin < occurrences.size();) {
                std::size_t end = begin + 1u;
                while (end < occurrences.size() &&
                       occurrences[end].hash == occurrences[begin].hash) {
                    ++end;
                }
                ++unique_count;
                begin = end;
            }
            return unique_count;
        };
        std::size_t unique_occurrence_group_count = 0u;
        bool production_group_hashes_strictly_monotonic = false;
        const std::size_t old_query_group_offset_capacity = scratch.query_group_offsets_.capacity();
        const std::size_t old_query_membership_filter_word_capacity =
            scratch.query_membership_filter_word_capacity_;
        if constexpr (p_oracle || p_compact_direct) {
            if (occurrence_layout->packed()) {
                build_query_plans(scratch.packed_occurrences_);
            } else {
                build_query_plans(scratch.wide_occurrences_);
            }
            unique_occurrence_group_count = scratch.query_plans_.size();
            if constexpr (p_compact_direct) {
                batch.compact_direct_query_set_stats_ = compact_direct_query_set_stats{
                    .query_count = queries.size(),
                    .query_occurrence_count = occurrence_count,
                    .unique_hash_count = unique_occurrence_group_count,
                    .group_key_set_checksum = compact_direct_group_key_set_checksum,
                    .present = true,
                };
            }
        } else {
            scratch.query_plan_reserve_request_ = 0u;
            unique_occurrence_group_count =
                occurrence_layout->packed()
                    ? count_unique_occurrence_groups(scratch.packed_occurrences_)
                    : count_unique_occurrence_groups(scratch.wide_occurrences_);
            if (unique_occurrence_group_count > (std::numeric_limits<std::uint32_t>::max)() ||
                unique_occurrence_group_count == (std::numeric_limits<std::size_t>::max)()) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native unique occurrence group count overflow");
            }
            if (occurrence_count != 0u) {
                scratch.ensure_vector_capacity_(scratch.query_group_offsets_,
                                                unique_occurrence_group_count + 1u);
                scratch.query_group_offsets_.resize(unique_occurrence_group_count + 1u);
                scratch.ensure_query_membership_filter_capacity_();
                const auto build_group_index =
                    [this, &scratch, &production_group_hashes_strictly_monotonic](
                        const auto& occurrences) noexcept {
                        std::size_t group_ordinal = 0u;
                        bool have_previous_hash = false;
                        std::uint32_t previous_hash = 0u;
                        bool strictly_monotonic = true;
                        for (std::size_t begin = 0u; begin < occurrences.size();) {
                            std::size_t end = begin + 1u;
                            while (end < occurrences.size() &&
                                   occurrences[end].hash == occurrences[begin].hash) {
                                ++end;
                            }
                            scratch.query_group_offsets_[group_ordinal++] =
                                static_cast<std::uint32_t>(begin);
                            const std::uint32_t hash = occurrences[begin].hash;
                            strictly_monotonic =
                                strictly_monotonic && (!have_previous_hash || previous_hash < hash);
                            previous_hash = hash;
                            have_previous_hash = true;
                            scratch.query_membership_filter_[query_membership_filter_word_(hash)] |=
                                query_membership_filter_mask_(hash);
                            begin = end;
                        }
                        scratch.query_group_offsets_[group_ordinal] =
                            static_cast<std::uint32_t>(occurrences.size());
                        production_group_hashes_strictly_monotonic =
                            strictly_monotonic && have_previous_hash;
                        return group_ordinal;
                    };
                const std::size_t built_group_count =
                    occurrence_layout->packed() ? build_group_index(scratch.packed_occurrences_)
                                                : build_group_index(scratch.wide_occurrences_);
                if (built_group_count != unique_occurrence_group_count ||
                    !production_group_hashes_strictly_monotonic) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native occurrence group receipt disagrees");
                }
                const auto group_hash = [&scratch,
                                         occurrence_layout](std::size_t group_ordinal) noexcept {
                    const std::uint32_t occurrence_ordinal =
                        scratch.query_group_offsets_[group_ordinal];
                    return occurrence_layout->packed()
                               ? scratch.packed_occurrences_[occurrence_ordinal].hash
                               : scratch.wide_occurrences_[occurrence_ordinal].hash;
                };
                std::uint32_t group_cursor = 0u;
                for (std::uint32_t bucket = 0u; bucket <= query_radix_bucket_count_; ++bucket) {
                    while (group_cursor < unique_occurrence_group_count &&
                           query_bucket_(group_hash(group_cursor)) < bucket) {
                        ++group_cursor;
                    }
                    scratch.query_plan_radix_[bucket] = group_cursor;
                }
            }
        }
        batch.stats_.query_plan_allocations =
            ((p_oracle || p_compact_direct) &&
             scratch.query_plans_.capacity() > old_query_plan_capacity)
                ? 1u
                : 0u;
        batch.stats_.peak_query_plan_capacity_bytes =
            (p_oracle || p_compact_direct)
                ? saturating_mul(scratch.query_plans_.capacity(), sizeof(query_hash_plan))
                : 0u;
        batch.stats_.query_group_offset_allocations =
            use_bucket_direct_matcher &&
                    scratch.query_group_offsets_.capacity() > old_query_group_offset_capacity
                ? 1u
                : 0u;
        batch.stats_.peak_query_group_offset_capacity_bytes =
            use_bucket_direct_matcher
                ? saturating_mul(scratch.query_group_offsets_.capacity(), sizeof(std::uint32_t))
                : 0u;
        batch.stats_.query_membership_filter_allocations =
            use_bucket_direct_matcher && scratch.query_membership_filter_word_capacity_ >
                                             old_query_membership_filter_word_capacity
                ? 1u
                : 0u;
        batch.stats_.peak_query_membership_filter_capacity_bytes =
            use_bucket_direct_matcher
                ? saturating_mul(scratch.query_membership_filter_word_capacity_,
                                 sizeof(std::uint64_t))
                : 0u;
        batch.stats_.unique_query_plans = unique_occurrence_group_count;
        batch.stats_.unique_plan_build_ns = elapsed_stage_ns_(unique_plan_started_ns);

        if constexpr (p_compact_direct) {
            prepare_compact_direct_anchors_(
                admission, queries, static_cast<std::uint32_t>(max_occ), scratch, batch);
        } else if (occurrence_count != 0u) {
            const std::size_t radix_size = static_cast<std::size_t>(query_radix_bucket_count_) + 1u;
            if constexpr (p_oracle) {
                const std::uint64_t query_radix_fill_started_ns = stage_now_ns_();
                const std::size_t old_query_radix_capacity = scratch.query_plan_radix_.capacity();
                scratch.ensure_vector_capacity_(scratch.query_plan_radix_, radix_size);
                batch.stats_.query_radix_allocations =
                    scratch.query_plan_radix_.capacity() > old_query_radix_capacity ? 1u : 0u;
                batch.stats_.peak_query_radix_capacity_bytes =
                    saturating_mul(scratch.query_plan_radix_.capacity(), sizeof(std::uint32_t));
                scratch.query_plan_radix_.resize(radix_size);
                std::uint32_t plan_cursor = 0u;
                const std::uint32_t radix_item_count =
                    static_cast<std::uint32_t>(scratch.query_plans_.size());
                for (std::uint32_t bucket = 0u; bucket <= query_radix_bucket_count_; ++bucket) {
                    while (plan_cursor < radix_item_count &&
                           query_bucket_(scratch.query_plans_[plan_cursor].hash) < bucket) {
                        ++plan_cursor;
                    }
                    scratch.query_plan_radix_[bucket] = plan_cursor;
                }
                batch.stats_.query_plan_radix_fill_ns =
                    elapsed_stage_ns_(query_radix_fill_started_ns);
            }
            const std::size_t old_occurrence_count_capacity = scratch.occurrence_count_capacity_;
            scratch.ensure_occurrence_count_capacity_(p_oracle ? scratch.query_plans_.size()
                                                               : unique_occurrence_group_count);
            batch.stats_.occurrence_count_allocations =
                scratch.occurrence_count_capacity_ > old_occurrence_count_capacity ? 1u : 0u;
            batch.stats_.peak_occurrence_count_capacity_bytes = saturating_mul(
                scratch.occurrence_count_capacity_, sizeof(std::atomic<std::uint32_t>));
            scratch.bucket_direct_matcher_active_ = use_bucket_direct_matcher;

            bucket_direct_matcher_view bucket_direct_matcher_storage{};
            const bucket_direct_matcher_view* bucket_direct_matcher = nullptr;
            if constexpr (!p_oracle) {
                const bool route_storage_matches =
                    occurrence_layout->packed()
                        ? scratch.packed_occurrences_.size() == occurrence_count &&
                              scratch.wide_occurrences_.empty() &&
                              scratch.packed_occurrences_.data() != nullptr
                        : scratch.wide_occurrences_.size() == occurrence_count &&
                              scratch.packed_occurrences_.empty() &&
                              scratch.wide_occurrences_.data() != nullptr;
                if (!production_group_hashes_strictly_monotonic ||
                    !valid_query_occurrence_packing_layout(scratch.occurrence_layout_) ||
                    scratch.occurrence_layout_ != *occurrence_layout || !route_storage_matches ||
                    scratch.query_membership_filter_ == nullptr ||
                    scratch.query_membership_filter_word_capacity_ !=
                        query_wave_scratch::query_membership_filter_word_count_ ||
                    scratch.query_plan_radix_.size() != radix_size ||
                    scratch.query_plan_radix_.data() == nullptr ||
                    scratch.query_plan_radix_.front() != 0u ||
                    scratch.query_plan_radix_.back() != unique_occurrence_group_count ||
                    scratch.query_group_offsets_.size() != unique_occurrence_group_count + 1u ||
                    scratch.query_group_offsets_.data() == nullptr ||
                    scratch.query_group_offsets_.front() != 0u ||
                    scratch.query_group_offsets_.back() != occurrence_count ||
                    scratch.active_occurrence_count_ != unique_occurrence_group_count ||
                    scratch.occurrence_count_capacity_ < unique_occurrence_group_count ||
                    scratch.occurrence_counts_ == nullptr) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native production matcher view receipt disagrees");
                }
                bucket_direct_matcher_storage = bucket_direct_matcher_view{
                    .membership_filter = scratch.query_membership_filter_.get(),
                    .radix = scratch.query_plan_radix_.data(),
                    .group_offsets = scratch.query_group_offsets_.data(),
                    .packed_occurrences = scratch.packed_occurrences_.data(),
                    .wide_occurrences = scratch.wide_occurrences_.data(),
                    .occurrence_counts = scratch.occurrence_counts_.get(),
                    .membership_filter_word_count = scratch.query_membership_filter_word_capacity_,
                    .radix_count = scratch.query_plan_radix_.size(),
                    .group_offset_count = scratch.query_group_offsets_.size(),
                    .packed_occurrence_count = scratch.packed_occurrences_.size(),
                    .wide_occurrence_count = scratch.wide_occurrences_.size(),
                    .occurrence_count = occurrence_count,
                    .counter_count = scratch.active_occurrence_count_,
                    .occurrence_layout = scratch.occurrence_layout_,
                };
                bucket_direct_matcher = &bucket_direct_matcher_storage;
                batch.stats_.bucket_direct_matcher_view_receipts = 1u;
            }

            const std::uint32_t max_occ_u32 = static_cast<std::uint32_t>(max_occ);
            std::atomic<std::uint32_t> first_error{0u};

            // Pass 1: controls and saturated counts only.
            const std::uint64_t control_count_started_ns = stage_now_ns_();
            parallel_entries_(
                block_count_,
                [this,
                 &scratch,
                 &first_error,
                 max_occ_u32,
                 use_fixed_h32_c16_p16,
                 bucket_direct_matcher](std::size_t block_begin, std::size_t block_end) noexcept {
                    const std::span<const query_hash_plan> plans{scratch.query_plans_};
                    const std::span<const std::uint32_t> radix{scratch.query_plan_radix_};
                    for (std::size_t block_ordinal = block_begin; block_ordinal < block_end;
                         ++block_ordinal) {
                        if (first_error.load(std::memory_order_relaxed) != 0u) {
                            return;
                        }
                        std::expected<void, minimizer_generation_encoding_error> counted{};
                        fragment_probe_context context{
                            .pass = fragment_probe_pass::control_count,
                            .block = &block_plans_[block_ordinal],
                            .block_ordinal = block_ordinal,
                            .plans = plans,
                            .query_plan_radix = radix,
                            .scratch = &scratch,
                            .bucket_direct_matcher = bucket_direct_matcher,
                            .max_occ = max_occ_u32,
                        };
                        if (use_fixed_h32_c16_p16) {
                            counted = probe_block_h32_c16_p16_<p_oracle>(context);
                        } else {
                            counted = visit_matched_control_counts_<p_oracle>(
                                block_plans_[block_ordinal],
                                context,
                                [&context, max_occ_u32](const query_occurrence_group& group,
                                                        std::uint64_t local_count) noexcept
                                    -> std::optional<minimizer_generation_encoding_error> {
                                    (void)reserve_query_occurrences_(
                                        occurrence_counter_<p_oracle>(context,
                                                                      group.counter_ordinal),
                                        local_count,
                                        max_occ_u32);
                                    return std::nullopt;
                                });
                        }
                        if (!counted) {
                            record_error_(first_error, counted.error());
                            return;
                        }
                    }
                });
            if (const auto error = recorded_error_(first_error)) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native control-count pass failed; wire_error=" +
                    std::to_string(static_cast<unsigned>(*error)));
            }
            batch.stats_.control_count_pass_ns = elapsed_stage_ns_(control_count_started_ns);
            batch.stats_.control_count_bytes = total_control_count_scan_bytes_;

            bool have_matched_controls = false;
            fragment_probe_context counted_group_context{
                .scratch = &scratch,
                .bucket_direct_matcher = bucket_direct_matcher,
            };
            const auto consume_counted_group =
                [this,
                 &batch,
                 &counted_group_context,
                 queries,
                 max_occ_u32,
                 &have_matched_controls](const query_occurrence_group& group) {
                    const std::uint32_t global_count =
                        occurrence_counter_<p_oracle>(counted_group_context, group.counter_ordinal)
                            .load(std::memory_order_relaxed);
                    have_matched_controls = have_matched_controls || global_count != 0u;
                    if (global_count == 0u || global_count > max_occ_u32) {
                        return;
                    }
                    const auto occurrence_error = with_query_group_occurrences_<p_oracle>(
                        counted_group_context,
                        group,
                        [this, &batch, queries, global_count](
                            const auto occurrences,
                            const query_occurrence_packing_layout& layout) noexcept
                            -> std::optional<minimizer_generation_encoding_error> {
                            for (const auto& occurrence : occurrences) {
                                const query_occurrence_reference item =
                                    query_occurrence_reference_(occurrence, layout);
                                if (const auto error = add_expected_anchor_count_(
                                        item, queries, batch, global_count)) {
                                    return error;
                                }
                            }
                            return std::nullopt;
                        });
                    if (occurrence_error) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native expected anchor count overflow");
                    }
                };
            if constexpr (p_oracle) {
                for (std::size_t plan_ordinal = 0u; plan_ordinal < scratch.query_plans_.size();
                     ++plan_ordinal) {
                    query_hash_plan& plan = scratch.query_plans_[plan_ordinal];
                    const query_occurrence_group group{
                        .hash = plan.hash,
                        .occurrence_ref_begin = plan.occurrence_ref_begin,
                        .occurrence_ref_count = plan.occurrence_ref_count,
                        .counter_ordinal = static_cast<std::uint32_t>(plan_ordinal),
                    };
                    plan.global_occurrence_count =
                        occurrence_counter_<p_oracle>(counted_group_context,
                                                      static_cast<std::uint32_t>(plan_ordinal))
                            .load(std::memory_order_relaxed);
                    consume_counted_group(group);
                }
            } else {
                DEBUG_ASSERT(bucket_direct_matcher != nullptr,
                             "production matcher view is prevalidated");
                const bucket_direct_matcher_view& view = *bucket_direct_matcher;
                const auto consume_indexed_groups =
                    [&view, &consume_counted_group](const auto* occurrences,
                                                    std::size_t occurrence_count) {
                        for (std::size_t group_ordinal = 0u;
                             group_ordinal + 1u < view.group_offset_count;
                             ++group_ordinal) {
                            const std::uint32_t begin = view.group_offsets[group_ordinal];
                            const std::uint32_t end = view.group_offsets[group_ordinal + 1u];
                            DEBUG_ASSERT(end > begin && end <= occurrence_count,
                                         "production group extent is prevalidated");
                            consume_counted_group(query_occurrence_group{
                                .hash = occurrences[begin].hash,
                                .occurrence_ref_begin = begin,
                                .occurrence_ref_count = end - begin,
                                .counter_ordinal = static_cast<std::uint32_t>(group_ordinal),
                            });
                        }
                    };
                if (view.occurrence_layout.packed()) {
                    consume_indexed_groups(view.packed_occurrences, view.packed_occurrence_count);
                } else {
                    consume_indexed_groups(view.wide_occurrences, view.wide_occurrence_count);
                }
            }

            const bool have_accepted_anchors = std::ranges::any_of(
                std::span<const prepared_query_anchors>{batch.queries_.get(), batch.query_count_},
                [](const prepared_query_anchors& query) noexcept {
                    return query.anchor_count != 0u;
                });
            if (!have_accepted_anchors) {
                // Matched heavy controls still require prefix/count
                // authentication, but no layout, position walk, final
                // allocation, emission pass, or sort is needed.
                if (have_matched_controls) {
                    const std::uint64_t rejected_validation_started_ns = stage_now_ns_();
                    first_error.store(0u, std::memory_order_relaxed);
                    std::atomic<std::uint64_t> rejected_prefix_bytes{0u};
                    std::atomic<std::uint64_t> rejected_matched_count_bytes{0u};
                    parallel_entries_(
                        block_count_,
                        [this,
                         &scratch,
                         &first_error,
                         &rejected_prefix_bytes,
                         &rejected_matched_count_bytes,
                         max_occ_u32,
                         use_fixed_h32_c16_p16,
                         bucket_direct_matcher](std::size_t block_begin,
                                                std::size_t block_end) noexcept {
                            const std::span<const query_hash_plan> plans{scratch.query_plans_};
                            const std::span<const std::uint32_t> radix{scratch.query_plan_radix_};
                            for (std::size_t block_ordinal = block_begin; block_ordinal < block_end;
                                 ++block_ordinal) {
                                if (first_error.load(std::memory_order_relaxed) != 0u) {
                                    return;
                                }
                                std::uint64_t block_matched_count_bytes = 0u;
                                std::uint64_t block_prefix_bytes = 0u;
                                std::expected<void, minimizer_generation_encoding_error>
                                    validated{};
                                fragment_probe_context context{
                                    .pass = fragment_probe_pass::rejected_validation,
                                    .block = &block_plans_[block_ordinal],
                                    .block_ordinal = block_ordinal,
                                    .plans = plans,
                                    .query_plan_radix = radix,
                                    .scratch = &scratch,
                                    .bucket_direct_matcher = bucket_direct_matcher,
                                    .max_occ = max_occ_u32,
                                    .matched_count_bytes = &block_matched_count_bytes,
                                    .rejected_prefix_bytes = &block_prefix_bytes,
                                };
                                if (use_fixed_h32_c16_p16) {
                                    validated = probe_block_h32_c16_p16_<p_oracle>(context);
                                } else {
                                    validated = visit_matched_controls_<p_oracle>(
                                        block_plans_[block_ordinal],
                                        context,
                                        [](std::size_t,
                                           const query_occurrence_group&,
                                           const grouped_soa_decoded_control&) noexcept
                                            -> std::optional<minimizer_generation_encoding_error> {
                                            return minimizer_generation_encoding_error::
                                                partition_id;
                                        },
                                        nullptr,
                                        &block_prefix_bytes);
                                }
                                if (!validated) {
                                    record_error_(first_error, validated.error());
                                    return;
                                }
                                if (!add_atomic_checked_(rejected_matched_count_bytes,
                                                         block_matched_count_bytes) ||
                                    !add_atomic_checked_(rejected_prefix_bytes,
                                                         block_prefix_bytes)) {
                                    record_error_(
                                        first_error,
                                        minimizer_generation_encoding_error::arithmetic_overflow);
                                    return;
                                }
                            }
                        });
                    if (const auto error = recorded_error_(first_error)) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native rejected-control validation failed; "
                            "wire_error=" +
                            std::to_string(static_cast<unsigned>(*error)));
                    }
                    batch.stats_.rejected_control_validation_ns =
                        elapsed_stage_ns_(rejected_validation_started_ns);
                    batch.stats_.rejected_control_validation_bytes =
                        use_fixed_h32_c16_p16 ? total_control_hash_scan_bytes_
                                              : total_control_count_scan_bytes_;
                    if (!add_checked_(
                            batch.stats_.rejected_control_validation_bytes,
                            rejected_matched_count_bytes.load(std::memory_order_relaxed)) ||
                        !add_checked_(batch.stats_.rejected_control_validation_bytes,
                                      rejected_prefix_bytes.load(std::memory_order_relaxed))) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native rejected-control byte "
                            "accounting overflow");
                    }
                }
            } else {

                // Layout state is activated only after Pass 1 establishes the
                // exact accepted anchor counts. This keeps failed count waves
                // from publishing/zeroing partially active layout owners.
                const std::uint64_t orientation_layout_started_ns = stage_now_ns_();
                if (queries.size() != 0u &&
                    block_count_ > (std::numeric_limits<std::size_t>::max)() / queries.size()) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native block/query layout overflow");
                }
                const std::size_t layout_count = block_count_ * queries.size();
                const std::size_t old_block_layout_capacity =
                    scratch.block_query_layouts_.capacity();
                scratch.ensure_vector_capacity_(scratch.block_query_layouts_, layout_count);
                batch.stats_.block_layout_allocations =
                    scratch.block_query_layouts_.capacity() > old_block_layout_capacity ? 1u : 0u;
                batch.stats_.peak_block_layout_capacity_bytes = saturating_mul(
                    scratch.block_query_layouts_.capacity(), sizeof(block_query_anchor_layout));
                scratch.block_query_layouts_.resize(layout_count);
                std::fill(scratch.block_query_layouts_.begin(),
                          scratch.block_query_layouts_.end(),
                          block_query_anchor_layout{});
                const std::size_t old_block_stats_capacity = scratch.block_stats_.capacity();
                scratch.ensure_vector_capacity_(scratch.block_stats_, block_count_);
                batch.stats_.block_stats_allocations =
                    scratch.block_stats_.capacity() > old_block_stats_capacity ? 1u : 0u;
                batch.stats_.peak_block_stats_capacity_bytes = saturating_mul(
                    scratch.block_stats_.capacity(), sizeof(fragment_block_wave_stats));
                scratch.block_stats_.resize(block_count_);
                std::fill(scratch.block_stats_.begin(),
                          scratch.block_stats_.end(),
                          fragment_block_wave_stats{});

                // Pass 2: accepted positions only, counting orientation.
                first_error.store(0u, std::memory_order_relaxed);
                parallel_entries_(
                    block_count_,
                    [this,
                     queries,
                     &scratch,
                     &first_error,
                     max_occ_u32,
                     use_fixed_h32_c16_p16,
                     bucket_direct_matcher](std::size_t block_begin,
                                            std::size_t block_end) noexcept {
                        const std::span<const query_hash_plan> plans{scratch.query_plans_};
                        const std::span<const std::uint32_t> radix{scratch.query_plan_radix_};
                        for (std::size_t block_ordinal = block_begin; block_ordinal < block_end;
                             ++block_ordinal) {
                            if (first_error.load(std::memory_order_relaxed) != 0u) {
                                return;
                            }
                            std::uint64_t block_matched_count_bytes = 0u;
                            std::uint64_t block_accepted_prefix_bytes = 0u;
                            std::uint64_t block_rejected_prefix_bytes = 0u;
                            std::uint64_t block_position_bytes = 0u;
                            std::expected<void, minimizer_generation_encoding_error> counted{};
                            fragment_probe_context context{
                                .pass = fragment_probe_pass::orientation,
                                .block = &block_plans_[block_ordinal],
                                .block_ordinal = block_ordinal,
                                .plans = plans,
                                .query_plan_radix = radix,
                                .queries = queries,
                                .scratch = &scratch,
                                .bucket_direct_matcher = bucket_direct_matcher,
                                .max_occ = max_occ_u32,
                                .matched_count_bytes = &block_matched_count_bytes,
                                .accepted_prefix_bytes = &block_accepted_prefix_bytes,
                                .rejected_prefix_bytes = &block_rejected_prefix_bytes,
                                .position_bytes = &block_position_bytes,
                            };
                            if (use_fixed_h32_c16_p16) {
                                counted = probe_block_h32_c16_p16_<p_oracle>(context);
                            } else {
                                counted = visit_matched_controls_<p_oracle>(
                                    block_plans_[block_ordinal],
                                    context,
                                    [this,
                                     queries,
                                     &scratch,
                                     block_ordinal,
                                     &block_position_bytes,
                                     &context](std::size_t entry_ordinal,
                                               const query_occurrence_group& group,
                                               const grouped_soa_decoded_control& control) noexcept
                                        -> std::optional<minimizer_generation_encoding_error> {
                                        const entry_plan& planned_entry =
                                            entry_plans_[entry_ordinal];
                                        auto position_cursor = position_range_cursor::create(
                                            mapped_payload_, control, planned_entry);
                                        if (!position_cursor) {
                                            return position_cursor.error();
                                        }
                                        std::uint64_t target_strand_zero = 0u;
                                        std::uint64_t target_strand_one = 0u;
                                        std::uint64_t packed = 0u;
                                        while (position_cursor->next_dynamic(packed)) {
                                            std::uint64_t& count = (packed & 1u) == 0u
                                                                       ? target_strand_zero
                                                                       : target_strand_one;
                                            if (count ==
                                                (std::numeric_limits<std::uint64_t>::max)()) {
                                                return minimizer_generation_encoding_error::
                                                    arithmetic_overflow;
                                            }
                                            ++count;
                                        }
                                        if (const auto error = position_cursor->error()) {
                                            return error;
                                        }
                                        std::uint64_t observed = 0u;
                                        if (!add_checked_(observed, target_strand_zero) ||
                                            !add_checked_(observed, target_strand_one) ||
                                            observed !=
                                                control.position_end - control.position_begin) {
                                            return minimizer_generation_encoding_error::
                                                arithmetic_overflow;
                                        }
                                        const std::uint64_t position_bytes =
                                            bioseq::common::saturating_mul(
                                                observed, planned_entry.position_byte_width);
                                        if (position_bytes ==
                                                (std::numeric_limits<std::uint64_t>::max)() ||
                                            !add_checked_(block_position_bytes, position_bytes)) {
                                            return minimizer_generation_encoding_error::
                                                arithmetic_overflow;
                                        }

                                        const orientation_occurrence_context occurrence_context{
                                            .queries = queries,
                                            .scratch = &scratch,
                                            .layout_base = block_ordinal * queries.size(),
                                            .target_strand_zero = target_strand_zero,
                                            .target_strand_one = target_strand_one,
                                        };
                                        return with_query_group_occurrences_<p_oracle>(
                                            context,
                                            group,
                                            [&occurrence_context](
                                                const auto occurrences,
                                                const query_occurrence_packing_layout&
                                                    packing_layout) noexcept
                                                -> std::optional<
                                                    minimizer_generation_encoding_error> {
                                                for (const auto& occurrence : occurrences) {
                                                    const query_occurrence_reference item =
                                                        query_occurrence_reference_(occurrence,
                                                                                    packing_layout);
                                                    if (const auto error = accumulate_orientation_(
                                                            item, occurrence_context)) {
                                                        return error;
                                                    }
                                                }
                                                return std::nullopt;
                                            });
                                    },
                                    &block_accepted_prefix_bytes,
                                    &block_rejected_prefix_bytes);
                            }
                            if (!counted) {
                                record_error_(first_error, counted.error());
                                return;
                            }
                            if (block_ordinal >= scratch.block_stats_.size()) {
                                record_error_(
                                    first_error,
                                    minimizer_generation_encoding_error::arithmetic_overflow);
                                return;
                            }
                            scratch.block_stats_[block_ordinal].pass2_matched_count_bytes =
                                block_matched_count_bytes;
                            scratch.block_stats_[block_ordinal].pass2_accepted_prefix_bytes =
                                block_accepted_prefix_bytes;
                            scratch.block_stats_[block_ordinal].pass2_rejected_prefix_bytes =
                                block_rejected_prefix_bytes;
                            scratch.block_stats_[block_ordinal].pass2_position_bytes =
                                block_position_bytes;
                        }
                    });
                if (const auto error = recorded_error_(first_error)) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native orientation-layout pass failed; "
                        "wire_error=" +
                        std::to_string(static_cast<unsigned>(*error)));
                }

                // Prefix [all forward blocks][all reverse blocks].
                for (std::size_t query_ordinal = 0u; query_ordinal < queries.size();
                     ++query_ordinal) {
                    std::uint64_t cursor = 0u;
                    for (std::size_t block_ordinal = 0u; block_ordinal < block_count_;
                         ++block_ordinal) {
                        block_query_anchor_layout& layout =
                            scratch.block_query_layouts_[block_ordinal * queries.size() +
                                                         query_ordinal];
                        layout.forward_begin = cursor;
                        layout.forward_cursor = cursor;
                        if (!add_checked_(cursor, layout.forward_count)) {
                            throw persisted_minimizer_lookup_error(
                                "fragment-native forward anchor prefix overflow");
                        }
                    }
                    for (std::size_t block_ordinal = 0u; block_ordinal < block_count_;
                         ++block_ordinal) {
                        block_query_anchor_layout& layout =
                            scratch.block_query_layouts_[block_ordinal * queries.size() +
                                                         query_ordinal];
                        layout.reverse_begin = cursor;
                        layout.reverse_cursor = cursor;
                        if (!add_checked_(cursor, layout.reverse_count)) {
                            throw persisted_minimizer_lookup_error(
                                "fragment-native reverse anchor prefix overflow");
                        }
                    }
                    if (cursor != batch.queries_[query_ordinal].anchor_count ||
                        cursor > (std::numeric_limits<std::size_t>::max)()) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native anchor layout count disagrees");
                    }
                }

                std::uint64_t final_owned_bytes = batch.owned_bytes_;
                for (std::size_t query_ordinal = 0u; query_ordinal < queries.size();
                     ++query_ordinal) {
                    const prepared_query_anchors& query = batch.queries_[query_ordinal];
                    const std::uint64_t anchor_bytes =
                        saturating_mul(query.anchor_count, sizeof(bioseq::chaining::anchor));
                    final_owned_bytes = saturating_add(final_owned_bytes, anchor_bytes);
                    if (anchor_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                        final_owned_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                        query.anchor_count > (std::numeric_limits<std::size_t>::max)() /
                                                 sizeof(bioseq::chaining::anchor)) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native exact anchor size overflow");
                    }
                }
                // Reserve the complete immutable result before allocating its
                // first per-query array. A later query can therefore never
                // discover an admission failure after earlier anchors exist.
                if (!batch.permit_.grow_to(final_owned_bytes)) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native exact anchor admission failed");
                }
                try {
                    for (std::size_t query_ordinal = 0u; query_ordinal < queries.size();
                         ++query_ordinal) {
                        prepared_query_anchors& query = batch.queries_[query_ordinal];
                        if (query.anchor_count == 0u)
                            continue;
                        query.anchors = std::make_unique_for_overwrite<bioseq::chaining::anchor[]>(
                            static_cast<std::size_t>(query.anchor_count));
                    }
                } catch (const std::bad_alloc&) {
                    // Batch destruction releases every earlier query array
                    // before returning the aggregate permit.
                    throw persisted_minimizer_lookup_error(
                        "fragment-native exact anchor allocation failed");
                }
                batch.owned_bytes_ = final_owned_bytes;
                batch.permit_.shrink_to(batch.owned_bytes_);
                batch.stats_.orientation_layout_ns =
                    elapsed_stage_ns_(orientation_layout_started_ns);

                // Pass 3: direct exact emission. Primary and tie sorting use
                // separate joined fragment barriers below so their wall times
                // remain independently attributable.
                const std::uint64_t direct_emission_started_ns = stage_now_ns_();
                first_error.store(0u, std::memory_order_relaxed);
                parallel_entries_(
                    block_count_,
                    [this,
                     queries,
                     &batch,
                     &scratch,
                     &first_error,
                     max_occ_u32,
                     use_fixed_h32_c16_p16,
                     bucket_direct_matcher](std::size_t block_begin,
                                            std::size_t block_end) noexcept {
                        const std::span<const query_hash_plan> plans{scratch.query_plans_};
                        const std::span<const std::uint32_t> radix{scratch.query_plan_radix_};
                        for (std::size_t block_ordinal = block_begin; block_ordinal < block_end;
                             ++block_ordinal) {
                            if (first_error.load(std::memory_order_relaxed) != 0u) {
                                return;
                            }
                            const fragment_block_plan& block = block_plans_[block_ordinal];
                            std::uint64_t block_matched_count_bytes = 0u;
                            std::uint64_t block_accepted_prefix_bytes = 0u;
                            std::uint64_t block_position_bytes = 0u;
                            std::uint64_t block_anchors_constructed = 0u;
                            std::expected<void, minimizer_generation_encoding_error> emitted{};
                            fragment_probe_context context{
                                .pass = fragment_probe_pass::emission,
                                .block = &block,
                                .block_ordinal = block_ordinal,
                                .plans = plans,
                                .query_plan_radix = radix,
                                .queries = queries,
                                .scratch = &scratch,
                                .bucket_direct_matcher = bucket_direct_matcher,
                                .batch = &batch,
                                .max_occ = max_occ_u32,
                                .matched_count_bytes = &block_matched_count_bytes,
                                .accepted_prefix_bytes = &block_accepted_prefix_bytes,
                                .position_bytes = &block_position_bytes,
                                .anchors_constructed = &block_anchors_constructed,
                            };
                            if (use_fixed_h32_c16_p16) {
                                emitted = probe_block_h32_c16_p16_<p_oracle>(context);
                            } else {
                                emitted = visit_matched_controls_<p_oracle>(
                                    block,
                                    context,
                                    [this,
                                     queries,
                                     &batch,
                                     &scratch,
                                     block_ordinal,
                                     &block,
                                     &block_position_bytes,
                                     &block_anchors_constructed,
                                     &context](std::size_t entry_ordinal,
                                               const query_occurrence_group& group,
                                               const grouped_soa_decoded_control& control) noexcept
                                        -> std::optional<minimizer_generation_encoding_error> {
                                        const entry_plan& planned_entry =
                                            entry_plans_[entry_ordinal];
                                        auto position_cursor = position_range_cursor::create(
                                            mapped_payload_, control, planned_entry);
                                        if (!position_cursor) {
                                            return position_cursor.error();
                                        }
                                        const direct_emission_occurrence_context occurrence_context{
                                            .queries = queries,
                                            .block = &block,
                                            .block_ordinal = block_ordinal,
                                            .scratch = &scratch,
                                            .batch = &batch,
                                        };
                                        const auto occurrence_error = with_query_group_occurrences_<
                                            p_oracle>(
                                            context,
                                            group,
                                            [this, &position_cursor, &occurrence_context](
                                                const auto occurrences,
                                                const query_occurrence_packing_layout&
                                                    packing_layout) noexcept
                                                -> std::optional<
                                                    minimizer_generation_encoding_error> {
                                                std::optional<minimizer_generation_encoding_error>
                                                    emit_error;
                                                std::uint64_t packed = 0u;
                                                while (position_cursor->next_dynamic(packed)) {
                                                    for (const auto& occurrence : occurrences) {
                                                        const query_occurrence_reference item =
                                                            query_occurrence_reference_(
                                                                occurrence, packing_layout);
                                                        if (const auto error =
                                                                emit_fragment_anchor_(
                                                                    packed,
                                                                    item,
                                                                    occurrence_context)) {
                                                            emit_error = error;
                                                            break;
                                                        }
                                                    }
                                                    if (emit_error)
                                                        break;
                                                }
                                                if (const auto error = position_cursor->error()) {
                                                    return error;
                                                }
                                                return emit_error;
                                            });
                                        if (occurrence_error) {
                                            return occurrence_error;
                                        }
                                        const std::uint64_t decoded_positions =
                                            control.position_end - control.position_begin;
                                        const std::uint64_t position_bytes =
                                            bioseq::common::saturating_mul(
                                                decoded_positions,
                                                planned_entry.position_byte_width);
                                        const std::uint64_t constructed =
                                            bioseq::common::saturating_mul(
                                                decoded_positions, group.occurrence_ref_count);
                                        if (position_bytes ==
                                                (std::numeric_limits<std::uint64_t>::max)() ||
                                            constructed ==
                                                (std::numeric_limits<std::uint64_t>::max)() ||
                                            !add_checked_(block_position_bytes, position_bytes) ||
                                            !add_checked_(block_anchors_constructed, constructed)) {
                                            return minimizer_generation_encoding_error::
                                                arithmetic_overflow;
                                        }
                                        return std::nullopt;
                                    },
                                    &block_accepted_prefix_bytes,
                                    nullptr,
                                    /*validate_rejected_prefixes=*/false);
                            }
                            if (!emitted) {
                                record_error_(first_error, emitted.error());
                                return;
                            }
                            if (block_ordinal >= scratch.block_stats_.size()) {
                                record_error_(
                                    first_error,
                                    minimizer_generation_encoding_error::arithmetic_overflow);
                                return;
                            }
                            scratch.block_stats_[block_ordinal].pass3_matched_count_bytes =
                                block_matched_count_bytes;
                            scratch.block_stats_[block_ordinal].pass3_accepted_prefix_bytes =
                                block_accepted_prefix_bytes;
                            scratch.block_stats_[block_ordinal].pass3_position_bytes =
                                block_position_bytes;
                            scratch.block_stats_[block_ordinal].anchors_constructed =
                                block_anchors_constructed;

                            for (std::size_t query_ordinal = 0u; query_ordinal < queries.size();
                                 ++query_ordinal) {
                                block_query_anchor_layout& layout =
                                    scratch.block_query_layouts_[block_ordinal * queries.size() +
                                                                 query_ordinal];
                                if (layout.forward_begin >
                                        (std::numeric_limits<std::uint64_t>::max)() -
                                            layout.forward_count ||
                                    layout.reverse_begin >
                                        (std::numeric_limits<std::uint64_t>::max)() -
                                            layout.reverse_count ||
                                    layout.forward_cursor !=
                                        layout.forward_begin + layout.forward_count ||
                                    layout.reverse_cursor !=
                                        layout.reverse_begin + layout.reverse_count) {
                                    record_error_(
                                        first_error,
                                        minimizer_generation_encoding_error::arithmetic_overflow);
                                    return;
                                }
                            }
                        }
                    });
                if (const auto error = recorded_error_(first_error)) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native direct anchor emission failed; "
                        "wire_error=" +
                        std::to_string(static_cast<unsigned>(*error)));
                }
                batch.stats_.direct_emission_ns = elapsed_stage_ns_(direct_emission_started_ns);

                const auto run_local_order_stage = [this, queries, &batch, &scratch, &first_error](
                                                       bool normalize_ties) {
                    first_error.store(0u, std::memory_order_relaxed);
                    parallel_entries_(
                        block_count_,
                        [queries,
                         &batch,
                         &scratch,
                         &first_error,
                         normalize_ties,
                         authenticated_target_length = manifest_.target_length](
                            std::size_t block_begin, std::size_t block_end) noexcept {
                            for (std::size_t block_ordinal = block_begin; block_ordinal < block_end;
                                 ++block_ordinal) {
                                if (first_error.load(std::memory_order_relaxed) != 0u ||
                                    block_ordinal >= scratch.block_stats_.size()) {
                                    if (block_ordinal >= scratch.block_stats_.size()) {
                                        record_error_(first_error,
                                                      minimizer_generation_encoding_error::
                                                          arithmetic_overflow);
                                    }
                                    return;
                                }
                                std::uint64_t block_order_bytes = 0u;
                                for (std::size_t query_ordinal = 0u; query_ordinal < queries.size();
                                     ++query_ordinal) {
                                    const block_query_anchor_layout& layout =
                                        scratch
                                            .block_query_layouts_[block_ordinal * queries.size() +
                                                                  query_ordinal];
                                    auto& query = batch.queries_[query_ordinal];
                                    const auto order_slice =
                                        [normalize_ties,
                                         &block_order_bytes,
                                         authenticated_target_length](
                                            bioseq::chaining::anchor* anchors,
                                            std::uint64_t count) noexcept -> bool {
                                        if (count < 2u)
                                            return true;
                                        const auto stats =
                                            normalize_ties
                                                ? normalize_anchor_slice_ties_(
                                                      anchors, static_cast<std::size_t>(count))
                                                : sort_anchor_slice_primary_(
                                                      anchors,
                                                      static_cast<std::size_t>(count),
                                                      authenticated_target_length);
                                        const std::uint64_t bytes =
                                            normalize_ties ? stats.tie_bytes : stats.primary_bytes;
                                        return !stats.saturated &&
                                               add_checked_(block_order_bytes, bytes);
                                    };
                                    if ((layout.forward_count > 1u &&
                                         !order_slice(
                                             query.anchors.get() +
                                                 static_cast<std::size_t>(layout.forward_begin),
                                             layout.forward_count)) ||
                                        (layout.reverse_count > 1u &&
                                         !order_slice(
                                             query.anchors.get() +
                                                 static_cast<std::size_t>(layout.reverse_begin),
                                             layout.reverse_count))) {
                                        record_error_(first_error,
                                                      minimizer_generation_encoding_error::
                                                          arithmetic_overflow);
                                        return;
                                    }
                                }
                                fragment_block_wave_stats& block_stats =
                                    scratch.block_stats_[block_ordinal];
                                if (normalize_ties) {
                                    block_stats.tie_normalization_bytes = block_order_bytes;
                                } else {
                                    block_stats.local_sort_bytes = block_order_bytes;
                                }
                            }
                        });
                    if (const auto error = recorded_error_(first_error)) {
                        throw persisted_minimizer_lookup_error(
                            std::string{normalize_ties
                                            ? "fragment-native tie normalization failed; "
                                            : "fragment-native local primary sort failed; "} +
                            "wire_error=" + std::to_string(static_cast<unsigned>(*error)));
                    }
                };

                const std::uint64_t local_sort_started_ns = stage_now_ns_();
                run_local_order_stage(/*normalize_ties=*/false);
                batch.stats_.local_sort_ns = elapsed_stage_ns_(local_sort_started_ns);
                const std::uint64_t tie_normalization_started_ns = stage_now_ns_();
                run_local_order_stage(/*normalize_ties=*/true);
                batch.stats_.tie_normalization_ns = elapsed_stage_ns_(tie_normalization_started_ns);

                std::uint64_t expected_constructed = 0u;
                for (std::size_t query_ordinal = 0u; query_ordinal < queries.size();
                     ++query_ordinal) {
                    if (!add_checked_(expected_constructed,
                                      batch.queries_[query_ordinal].anchor_count)) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native stats anchor total overflow");
                    }
                }
                batch.stats_.pass2_control_bytes = use_fixed_h32_c16_p16
                                                       ? total_control_hash_scan_bytes_
                                                       : total_control_count_scan_bytes_;
                batch.stats_.pass3_control_bytes = use_fixed_h32_c16_p16
                                                       ? total_control_hash_scan_bytes_
                                                       : total_control_count_scan_bytes_;
                for (const fragment_block_wave_stats& block_stats : scratch.block_stats_) {
                    if (!add_checked_(batch.stats_.pass2_control_bytes,
                                      block_stats.pass2_matched_count_bytes) ||
                        !add_checked_(batch.stats_.pass2_accepted_prefix_bytes,
                                      block_stats.pass2_accepted_prefix_bytes) ||
                        !add_checked_(batch.stats_.rejected_control_validation_bytes,
                                      block_stats.pass2_rejected_prefix_bytes) ||
                        !add_checked_(batch.stats_.pass2_position_bytes,
                                      block_stats.pass2_position_bytes) ||
                        !add_checked_(batch.stats_.pass3_control_bytes,
                                      block_stats.pass3_matched_count_bytes) ||
                        !add_checked_(batch.stats_.pass3_accepted_prefix_bytes,
                                      block_stats.pass3_accepted_prefix_bytes) ||
                        !add_checked_(batch.stats_.pass3_position_bytes,
                                      block_stats.pass3_position_bytes) ||
                        !add_checked_(batch.stats_.anchors_constructed,
                                      block_stats.anchors_constructed) ||
                        !add_checked_(batch.stats_.local_sort_bytes,
                                      block_stats.local_sort_bytes) ||
                        !add_checked_(batch.stats_.tie_normalization_bytes,
                                      block_stats.tie_normalization_bytes)) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native stats reduction overflow");
                    }
                }
                const std::uint64_t exact_final_bytes =
                    saturating_mul(expected_constructed, sizeof(bioseq::chaining::anchor));
                const bool control_byte_equation =
                    use_fixed_h32_c16_p16
                        ? batch.stats_.pass2_control_bytes >= total_control_hash_scan_bytes_ &&
                              batch.stats_.pass2_control_bytes <=
                                  batch.stats_.control_count_bytes &&
                              batch.stats_.pass3_control_bytes >= total_control_hash_scan_bytes_ &&
                              batch.stats_.pass3_control_bytes <=
                                  batch.stats_.pass2_control_bytes &&
                              (batch.stats_.pass2_control_bytes - total_control_hash_scan_bytes_) %
                                      sizeof(std::uint16_t) ==
                                  0u &&
                              (batch.stats_.pass3_control_bytes - total_control_hash_scan_bytes_) %
                                      sizeof(std::uint16_t) ==
                                  0u
                        : batch.stats_.pass2_control_bytes == batch.stats_.control_count_bytes &&
                              batch.stats_.pass3_control_bytes == batch.stats_.control_count_bytes;
                if (!control_byte_equation ||
                    batch.stats_.pass2_accepted_prefix_bytes !=
                        batch.stats_.pass3_accepted_prefix_bytes ||
                    batch.stats_.pass2_position_bytes != batch.stats_.pass3_position_bytes ||
                    batch.stats_.anchors_constructed != expected_constructed ||
                    exact_final_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native stats pass equation failed");
                }
                batch.stats_.exact_final_bytes = exact_final_bytes;
                batch.stats_.peak_exact_final_bytes = exact_final_bytes;
            }
        }

        // Compositional sortedness authority: the control-scan route fills
        // forward/reverse slices inside strictly ordered disjoint blocks;
        // the compact-direct route fills one exact forward and one exact
        // reverse slice per query. Both routes apply the same primary and
        // tie stages inside every resulting slice. canonical_anchor_less
        // encodes orientation as the ref_bits MSB, so the forward slices
        // followed by the reverse slices compose without a further O(A)
        // production traversal.
        batch.stats_.compositional_sortedness_receipts = queries.size();
#ifndef NDEBUG
        verify_compositional_order_for_testing = true;
#endif
        const auto less = bioseq::chaining::canonical_anchor_less{};
        for (std::size_t query_ordinal = 0u; query_ordinal < queries.size(); ++query_ordinal) {
            prepared_query_anchors& query = batch.queries_[query_ordinal];
            if (verify_compositional_order_for_testing) {
                const std::uint64_t verification_bytes =
                    saturating_mul(query.anchor_count, sizeof(bioseq::chaining::anchor));
                if (verification_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                    !add_checked_(batch.stats_.final_sortedness_validation_bytes,
                                  verification_bytes)) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native anchor order verification overflow");
                }
                for (std::size_t anchor_ordinal = 1u;
                     anchor_ordinal < static_cast<std::size_t>(query.anchor_count);
                     ++anchor_ordinal) {
                    if (less(query.anchors[anchor_ordinal], query.anchors[anchor_ordinal - 1u])) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native anchor order stats failed");
                    }
                }
            }
            query.sortedness_receipt = prepared_fragment_anchor_batch::sortedness_receipt_;
        }
        if (batch.owned_bytes_ == (std::numeric_limits<std::uint64_t>::max)() ||
            batch.owned_bytes_ > batch.permit_.bytes()) {
            throw persisted_minimizer_lookup_error("fragment-native anchor ownership stats failed");
        }
        if (!batch.stats_.forbidden_buffers_are_zero() ||
            batch.stats_.exact_final_bytes > batch.owned_bytes_) {
            throw persisted_minimizer_lookup_error("fragment-native stats ownership stats failed");
        }
        batch.permit_.shrink_to(batch.owned_bytes_);
        batch.ready_ = true;
        scratch.wave_state_.store(query_wave_scratch::wave_state::outstanding,
                                  std::memory_order_release);
        return batch;
    } catch (...) {
        scratch.reset_failed_wave_();
        throw;
    }
}

} // namespace bioseq::indexing
