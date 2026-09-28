/**
 * @file query_mapping.cppm
 * @brief Prepares query-hash lookups and the order for reading index extents.
 */
export module bioseq.indexing.minimizer_hashmap:prepared_reader_core_read_path_query_mapping;

import std;
import :exact_simd;
import :grouped_soa;
import :prepared_reader_directory;
import :prepared_reader_receipts;
import bioseq.chaining.core;
import bioseq.common.checked_arithmetic;
import bioseq.common.logging;
import bioseq.common.radix_sort;
import bioseq.common.seqan_adapter;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.type;
import bioseq.instrumentation;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.pipeline_ops;
import bioseq.parallelism.pool;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.lazy_sequence;
import bioseq.storage.scanner;
import bioseq.storage.size;
import :prepared_reader_core_read_path_declarations;
import :prepared_reader_core_read_path_admission;

export namespace bioseq::indexing {

namespace minimizer_prepared_reader_detail {

template <class p_traits>
[[nodiscard]] inline std::expected<
    typename trait_driven_prepared_query_core<p_traits>::query_radix_plan,
    minimizer_generation_encoding_error>
trait_driven_prepared_query_core<p_traits>::plan_query_radix_(std::size_t query_count,
                                                              std::uint64_t max_hash) noexcept {
    const std::uint32_t hash_bits = static_cast<std::uint32_t>(std::bit_width(max_hash));
    const std::size_t required_buckets = query_count / query_radix_target_mean_ +
                                         (query_count % query_radix_target_mean_ != 0u ? 1u : 0u);
    const std::uint32_t requested_bits =
        required_buckets <= 1u ? 0u
                               : static_cast<std::uint32_t>(std::bit_width(
                                     static_cast<std::uint64_t>(required_buckets - 1u)));
    const std::uint32_t radix_bits = (std::min)(hash_bits, requested_bits);
    if (radix_bits >= std::numeric_limits<std::size_t>::digits) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    const std::size_t bucket_count = std::size_t{1u} << radix_bits;
    if (bucket_count == (std::numeric_limits<std::size_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    const std::uint32_t suffix_bits = hash_bits - radix_bits;
    const std::uint64_t maximum_suffix = suffix_bits == 0u ? 0u
                                         : suffix_bits >= std::numeric_limits<std::uint64_t>::digits
                                             ? (std::numeric_limits<std::uint64_t>::max)()
                                             : (std::uint64_t{1u} << suffix_bits) - 1u;
    return query_radix_plan{
        .hash_bits = hash_bits,
        .radix_bits = radix_bits,
        .suffix_bits = suffix_bits,
        .suffix_width = prepared_query_scalar_width_for_max(maximum_suffix),
        .bucket_count = bucket_count,
        .offset_count = bucket_count + 1u,
    };
}

template <class p_traits>
[[nodiscard]] inline std::size_t trait_driven_prepared_query_core<p_traits>::query_radix_bucket_(
    hash_type hash, std::uint32_t hash_bits, std::uint32_t radix_bits) noexcept {
    if (radix_bits == 0u)
        return 0u;
    return static_cast<std::size_t>(static_cast<std::uint64_t>(hash) >> (hash_bits - radix_bits));
}

template <class p_traits>
[[nodiscard]] inline std::uint64_t trait_driven_prepared_query_core<p_traits>::query_radix_suffix_(
    hash_type hash, std::uint32_t suffix_bits) noexcept {
    if (suffix_bits == 0u)
        return 0u;
    const std::uint64_t value = static_cast<std::uint64_t>(hash);
    if (suffix_bits >= std::numeric_limits<std::uint64_t>::digits) {
        return value;
    }
    return value & ((std::uint64_t{1u} << suffix_bits) - 1u);
}

template <class p_traits>
[[nodiscard]] inline std::size_t
trait_driven_prepared_query_core<p_traits>::query_index_for_hash_impl_(
    std::uint64_t hash, std::uint32_t* hash_probes) const noexcept {
    if (hash_probes != nullptr)
        *hash_probes = 0u;
    if (hash > (std::numeric_limits<hash_type>::max)() || hash > this->profile_.max_hash) {
        return no_route;
    }
    const hash_type narrowed = static_cast<hash_type>(hash);
    if constexpr (p_traits::global_controls_then_positions) {
        if (!protocol_query_radix_offsets_ || protocol_query_radix_offset_count_ < 2u ||
            protocol_query_suffixes_.count != protocol_query_count_ ||
            protocol_query_exact_counts_.count != protocol_query_count_) {
            return no_route;
        }
        const std::size_t bucket =
            query_radix_bucket_(narrowed, protocol_query_hash_bits_, protocol_query_radix_bits_);
        if (bucket + 1u >= protocol_query_radix_offset_count_) {
            return no_route;
        }
        std::size_t first = protocol_query_radix_offsets_[bucket];
        const std::size_t end = protocol_query_radix_offsets_[bucket + 1u];
        if (first > end || end > protocol_query_count_) {
            return no_route;
        }
        const std::uint64_t wanted = query_radix_suffix_(narrowed, protocol_query_suffix_bits_);
        /// Search the stored hash suffixes within this  radix bucket.
        const auto search = [&first, &end, &wanted, &hash_probes](const auto* suffixes) noexcept {
            std::size_t lower = first;
            std::size_t upper = end;
            while (lower < upper) {
                const std::size_t middle = lower + (upper - lower) / 2u;
                if (hash_probes != nullptr)
                    ++*hash_probes;
                if (static_cast<std::uint64_t>(suffixes[middle]) < wanted) {
                    lower = middle + 1u;
                } else {
                    upper = middle;
                }
            }
            if (lower < end) {
                if (hash_probes != nullptr)
                    ++*hash_probes;
                if (static_cast<std::uint64_t>(suffixes[lower]) == wanted) {
                    return lower;
                }
            }
            return no_route;
        };
        switch (protocol_query_suffixes_.width) {
        case prepared_query_scalar_width::u8:
            return search(protocol_query_suffixes_.u8.get());
        case prepared_query_scalar_width::u16:
            return search(protocol_query_suffixes_.u16.get());
        case prepared_query_scalar_width::u32:
            return search(protocol_query_suffixes_.u32.get());
        case prepared_query_scalar_width::u64:
            return search(protocol_query_suffixes_.u64.get());
        }
        return no_route;
    } else {
        const auto found = std::lower_bound(
            protocol_query_hashes_.begin(), protocol_query_hashes_.end(), narrowed);
        return found != protocol_query_hashes_.end() && *found == narrowed
                   ? static_cast<std::size_t>(found - protocol_query_hashes_.begin())
                   : no_route;
    }
}

template <class p_traits>
[[nodiscard]] inline std::size_t trait_driven_prepared_query_core<p_traits>::query_index_for_hash(
    std::uint64_t hash) const noexcept {
    return query_index_for_hash_impl_(hash, nullptr);
}

template <class p_traits>
[[nodiscard]] inline std::size_t
trait_driven_prepared_query_core<p_traits>::query_index_for_control_hash(
    std::uint64_t hash, std::uint32_t& hash_probes) const noexcept {
    return query_index_for_hash_impl_(hash, &hash_probes);
}

template <class p_traits>
[[nodiscard]] inline bool trait_driven_prepared_query_core<p_traits>::query_intersects_entry(
    const grouped_soa_bucket_directory_entry& entry) const noexcept {
    const auto found = std::lower_bound(protocol_query_hashes_.begin(),
                                        protocol_query_hashes_.end(),
                                        static_cast<hash_type>(entry.lo_hash));
    return found != protocol_query_hashes_.end() &&
           static_cast<std::uint64_t>(*found) <= entry.hi_hash;
}

template <class p_traits>
inline void trait_driven_prepared_query_core<p_traits>::initialize_physical_cursor() noexcept {
    protocol_entry_index_ = 0;
    protocol_position_stage_ = false;
    protocol_segment_begin_ = 0;
    protocol_segment_end_ = 0;
    const auto entries = this->directory_.entries();
    if constexpr (!p_traits::global_controls_then_positions) {
        if (!entries.empty()) {
            protocol_segment_end_ = 1u;
            while (protocol_segment_end_ < entries.size() &&
                   entries[protocol_segment_end_].position_segment == entries[0].position_segment) {
                ++protocol_segment_end_;
            }
        }
    }
}

template <class p_traits>
inline void trait_driven_prepared_query_core<p_traits>::advance_physical_cursor() noexcept {
    const auto entries = this->directory_.entries();
    ++protocol_entry_index_;
    if constexpr (p_traits::global_controls_then_positions) {
        if (protocol_entry_index_ == entries.size() && !protocol_position_stage_) {
            protocol_position_stage_ = true;
            protocol_entry_index_ = 0;
        } else if (protocol_entry_index_ == entries.size()) {
            protocol_position_stage_ = false;
        }
        return;
    }
    if (protocol_entry_index_ != protocol_segment_end_)
        return;
    if (!protocol_position_stage_) {
        protocol_position_stage_ = true;
        protocol_entry_index_ = protocol_segment_begin_;
        return;
    }
    protocol_position_stage_ = false;
    protocol_segment_begin_ = protocol_segment_end_;
    protocol_entry_index_ = protocol_segment_begin_;
    if (protocol_segment_begin_ >= entries.size())
        return;
    protocol_segment_end_ = protocol_segment_begin_ + 1u;
    while (protocol_segment_end_ < entries.size() &&
           entries[protocol_segment_end_].position_segment ==
               entries[protocol_segment_begin_].position_segment) {
        ++protocol_segment_end_;
    }
}

template <class p_traits>
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
trait_driven_prepared_query_core<p_traits>::initialize_identity_core(
    bioseq::parallelism::memory_admission_controller& admission,
    const minimizer_generation_head& head,
    const minimizer_generation_manifest& manifest,
    const expected_minimizer_generation_identity& expected,
    const grouped_soa_minimizer_profile& profile,
    std::span<const std::byte> encoded_directory,
    std::uint64_t additional_logical_bytes) {
    return identity_core::begin_admitted_core(static_cast<identity_core&>(*this),
                                              admission,
                                              head,
                                              manifest,
                                              expected,
                                              profile,
                                              encoded_directory,
                                              additional_logical_bytes);
}

template <class p_traits>
[[nodiscard]] constexpr std::uint64_t
trait_driven_prepared_query_core<p_traits>::query_hash_value_(std::uint64_t hash) noexcept {
    return hash;
}

template <class p_traits>
[[nodiscard]] constexpr std::uint64_t
trait_driven_prepared_query_core<p_traits>::query_hash_value_(std::uint32_t hash) noexcept {
    return hash;
}

template <class p_traits>
[[nodiscard]] inline std::uint64_t trait_driven_prepared_query_core<p_traits>::query_hash_value_(
    const minimap_sketch_data& minimizer) noexcept {
    return minimizer.kmer();
}

template <class p_traits>
template <class p_query_value>
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
trait_driven_prepared_query_core<p_traits>::initialize_query_protocol(
    bioseq::parallelism::memory_admission_controller& admission,
    const minimizer_generation_head& head,
    const minimizer_generation_manifest& manifest,
    const expected_minimizer_generation_identity& expected,
    const grouped_soa_minimizer_profile& profile,
    std::span<const std::byte> encoded_directory,
    std::span<const p_query_value> query_hashes,
    std::uint64_t max_occ,
    grouped_soa_query_reader_options test_options,
    bool query_hashes_are_sorted_unique) {
    protocol_admission_ = &admission;
    if constexpr (p_traits::global_controls_then_positions) {
        protocol_test_options_ = test_options;
    }
    if (query_hashes.size() > (std::numeric_limits<std::size_t>::max)() / sizeof(hash_type)) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    std::uint64_t previous_hash = 0u;
    bool has_previous_hash = false;
    for (const p_query_value& query_value : query_hashes) {
        const std::uint64_t hash = query_hash_value_(query_value);
        if (hash > profile.max_hash || hash > (std::numeric_limits<hash_type>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::hash_range);
        }
        if (query_hashes_are_sorted_unique && has_previous_hash && hash <= previous_hash) {
            return std::unexpected(minimizer_generation_encoding_error::row_order);
        }
        previous_hash = hash;
        has_previous_hash = true;
    }
    const std::uint64_t query_bytes =
        bioseq::common::saturating_mul(query_hashes.size(), sizeof(hash_type));
    if (query_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    if (inject_admission_failure_(
            grouped_soa_query_admission_stage::identity_and_directory_reservation)) {
        capture_admission_failure_(
            grouped_soa_query_admission_stage::identity_and_directory_reservation,
            0u,
            0u,
            0u,
            0u,
            nullptr,
            true);
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }
    bioseq::parallelism::memory_admission_failure core_failure;
    if (auto admitted = identity_core::begin_admitted_core(static_cast<identity_core&>(*this),
                                                           admission,
                                                           head,
                                                           manifest,
                                                           expected,
                                                           profile,
                                                           encoded_directory,
                                                           query_bytes,
                                                           &core_failure);
        !admitted) {
        if (admitted.error() == minimizer_generation_encoding_error::admission) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::identity_and_directory_reservation,
                core_failure.requested_bytes,
                0u,
                0u,
                0u,
                &core_failure);
        }
        return std::unexpected(admitted.error());
    }
    if (!replace_ledger_field_(protocol_owned_bytes_.directory_bytes,
                               this->core_owned_capacity_bytes())) {
        return protocol_poison<void>(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    protocol_max_occ_ = max_occ;

    permit_growth_guard guard{*this};
    std::uint64_t projected = bioseq::common::saturating_add(
        protocol_owned_capacity_bytes(),
        bioseq::common::saturating_mul(
            query_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor));
    if (projected == (std::numeric_limits<std::uint64_t>::max)()) {
        return protocol_poison<void>(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    if (inject_admission_failure_(grouped_soa_query_admission_stage::query_hash_reservation)) {
        capture_admission_failure_(grouped_soa_query_admission_stage::query_hash_reservation,
                                   projected,
                                   0u,
                                   0u,
                                   query_bytes,
                                   nullptr,
                                   true);
        return protocol_poison<void>(minimizer_generation_encoding_error::admission);
    }
    bioseq::parallelism::memory_admission_failure query_hash_failure;
    if (!guard.grow(projected, &query_hash_failure)) {
        capture_admission_failure_(grouped_soa_query_admission_stage::query_hash_reservation,
                                   projected,
                                   0u,
                                   0u,
                                   query_bytes,
                                   &query_hash_failure);
        return protocol_poison<void>(minimizer_generation_encoding_error::admission);
    }
    if (inject_admission_failure_(grouped_soa_query_admission_stage::query_hash_allocation)) {
        capture_admission_failure_(grouped_soa_query_admission_stage::query_hash_allocation,
                                   projected,
                                   0u,
                                   0u,
                                   query_bytes,
                                   nullptr,
                                   true);
        return protocol_poison<void>(minimizer_generation_encoding_error::admission);
    }
    try {
        protocol_query_hashes_.reserve(query_hashes.size());
        for (const p_query_value& query_value : query_hashes) {
            const std::uint64_t hash = query_hash_value_(query_value);
            protocol_query_hashes_.push_back(static_cast<hash_type>(hash));
        }
        if (!query_hashes_are_sorted_unique) {
            std::ranges::sort(protocol_query_hashes_);
            protocol_query_hashes_.erase(
                std::unique(protocol_query_hashes_.begin(), protocol_query_hashes_.end()),
                protocol_query_hashes_.end());
        }
        if constexpr (p_traits::global_controls_then_positions) {
            // After deduplication, shrink the temporary full-hash vector to the unique
            // query count. The reservation covers both vectors while copying.
            // Release the full hashes after building suffixes and selecting extents.
            if (!query_hashes_are_sorted_unique) {
                std::vector<hash_type> canonical;
                canonical.reserve(protocol_query_hashes_.size());
                canonical.assign(protocol_query_hashes_.begin(), protocol_query_hashes_.end());
                protocol_query_hashes_.swap(canonical);
            }
        }
    } catch (const std::bad_alloc&) {
        capture_admission_failure_(grouped_soa_query_admission_stage::query_hash_allocation,
                                   projected,
                                   0u,
                                   0u,
                                   query_bytes);
        return protocol_poison<void>(minimizer_generation_encoding_error::admission);
    }
    const std::uint64_t actual_query_hash_bytes =
        bioseq::common::saturating_mul(protocol_query_hashes_.capacity(), sizeof(hash_type));
    if (actual_query_hash_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        !replace_ledger_field_(protocol_owned_bytes_.query_hash_bytes, actual_query_hash_bytes)) {
        return protocol_poison<void>(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    if (inject_admission_failure_(grouped_soa_query_admission_stage::query_hash_reconcile)) {
        capture_admission_failure_(grouped_soa_query_admission_stage::query_hash_reconcile,
                                   projected,
                                   0u,
                                   0u,
                                   query_bytes,
                                   nullptr,
                                   true);
        return protocol_poison<void>(minimizer_generation_encoding_error::admission);
    }
    if (!guard.reconcile()) {
        capture_admission_failure_(grouped_soa_query_admission_stage::query_hash_reconcile,
                                   projected,
                                   0u,
                                   0u,
                                   query_bytes);
        return protocol_poison<void>(minimizer_generation_encoding_error::admission);
    }
    if constexpr (p_traits::global_controls_then_positions) {
        // Reserve UINT32_MAX to mean that a control has no matching query.
        if (protocol_query_hashes_.size() >= no_narrow_route) {
            return protocol_poison<void>(minimizer_generation_encoding_error::arithmetic_overflow);
        }
    }

    query_radix_plan radix_plan{};
    prepared_query_scalar_width query_count_width = prepared_query_scalar_width::u8;
    if constexpr (p_traits::global_controls_then_positions) {
        const auto planned =
            plan_query_radix_(protocol_query_hashes_.size(), this->profile_.max_hash);
        if (!planned)
            return protocol_poison<void>(planned.error());
        radix_plan = *planned;
        query_count_width = prepared_query_scalar_width_for_max(this->manifest_.position_count);
    }

    const auto entries = this->directory_.entries();
    std::size_t selected_count = 0;
    for (const auto& entry : entries) {
        selected_count += query_intersects_entry(entry) ? 1u : 0u;
    }
    std::uint64_t route_vector_bytes = 0;
    route_vector_bytes = bioseq::common::saturating_add(
        route_vector_bytes,
        bioseq::common::saturating_mul(
            protocol_query_hashes_.size(),
            p_traits::global_controls_then_positions
                ? static_cast<std::size_t>(static_cast<std::uint8_t>(radix_plan.suffix_width) +
                                           static_cast<std::uint8_t>(query_count_width))
                : sizeof(query_record)));
    route_vector_bytes = bioseq::common::saturating_add(
        route_vector_bytes, bioseq::common::saturating_mul(entries.size(), sizeof(std::size_t)));
    route_vector_bytes = bioseq::common::saturating_add(
        route_vector_bytes,
        bioseq::common::saturating_mul(selected_count, sizeof(selected_extent)));
    const std::uint64_t query_radix_bytes =
        p_traits::global_controls_then_positions
            ? bioseq::common::saturating_mul(radix_plan.offset_count, sizeof(std::uint32_t))
            : 0u;
    const std::uint64_t route_bytes =
        bioseq::common::saturating_add(route_vector_bytes, query_radix_bytes);
    permit_growth_guard route_guard{*this};
    projected = bioseq::common::saturating_add(
        protocol_owned_capacity_bytes(),
        bioseq::common::saturating_add(
            bioseq::common::saturating_mul(
                route_vector_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor),
            query_radix_bytes));
    if (route_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        projected == (std::numeric_limits<std::uint64_t>::max)()) {
        return protocol_poison<void>(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    if (inject_admission_failure_(grouped_soa_query_admission_stage::query_route_reservation)) {
        capture_admission_failure_(grouped_soa_query_admission_stage::query_route_reservation,
                                   projected,
                                   0u,
                                   0u,
                                   route_bytes,
                                   nullptr,
                                   true);
        return protocol_poison<void>(minimizer_generation_encoding_error::admission);
    }
    bioseq::parallelism::memory_admission_failure route_failure;
    if (!route_guard.grow(projected, &route_failure)) {
        capture_admission_failure_(grouped_soa_query_admission_stage::query_route_reservation,
                                   projected,
                                   0u,
                                   0u,
                                   route_bytes,
                                   &route_failure);
        return protocol_poison<void>(minimizer_generation_encoding_error::admission);
    }
    if (inject_admission_failure_(grouped_soa_query_admission_stage::query_route_allocation)) {
        capture_admission_failure_(grouped_soa_query_admission_stage::query_route_allocation,
                                   projected,
                                   0u,
                                   0u,
                                   route_bytes,
                                   nullptr,
                                   true);
        return protocol_poison<void>(minimizer_generation_encoding_error::admission);
    }
    std::uint64_t released_full_query_hash_bytes = 0u;
    try {
        if constexpr (p_traits::global_controls_then_positions) {
            std::unique_ptr<std::uint32_t[]> radix_offsets;
            prepared_query_scalar_array suffixes;
            prepared_query_scalar_array exact_counts;
            radix_offsets =
                std::make_unique_for_overwrite<std::uint32_t[]>(radix_plan.offset_count);
            suffixes.allocate(radix_plan.suffix_width, protocol_query_hashes_.size(), false);
            exact_counts.allocate(query_count_width, protocol_query_hashes_.size(), true);
            std::size_t query_index = 0u;
            std::uint64_t nonempty_buckets = 0u;
            std::uint64_t max_bucket_size = 0u;
            for (std::size_t bucket = 0u; bucket < radix_plan.bucket_count; ++bucket) {
                radix_offsets[bucket] = static_cast<std::uint32_t>(query_index);
                const std::size_t bucket_begin = query_index;
                while (query_index < protocol_query_hashes_.size() &&
                       query_radix_bucket_(protocol_query_hashes_[query_index],
                                           radix_plan.hash_bits,
                                           radix_plan.radix_bits) == bucket) {
                    if (!suffixes.set(query_index,
                                      query_radix_suffix_(protocol_query_hashes_[query_index],
                                                          radix_plan.suffix_bits))) {
                        return protocol_poison<void>(
                            minimizer_generation_encoding_error::hash_range);
                    }
                    ++query_index;
                }
                const std::uint64_t bucket_size =
                    static_cast<std::uint64_t>(query_index - bucket_begin);
                nonempty_buckets += bucket_size != 0u ? 1u : 0u;
                max_bucket_size = (std::max)(max_bucket_size, bucket_size);
            }
            if (query_index != protocol_query_hashes_.size()) {
                return protocol_poison<void>(minimizer_generation_encoding_error::hash_range);
            }
            radix_offsets[radix_plan.bucket_count] = static_cast<std::uint32_t>(query_index);
            protocol_query_count_ = protocol_query_hashes_.size();
            protocol_query_suffixes_ = std::move(suffixes);
            protocol_query_exact_counts_ = std::move(exact_counts);
            protocol_query_radix_offsets_ = std::move(radix_offsets);
            protocol_query_radix_offset_count_ = radix_plan.offset_count;
            protocol_query_radix_bits_ = radix_plan.radix_bits;
            protocol_query_hash_bits_ = radix_plan.hash_bits;
            protocol_query_suffix_bits_ = radix_plan.suffix_bits;
            protocol_memory_stats_.query_route_radix_bucket_count = radix_plan.bucket_count;
            protocol_memory_stats_.query_route_radix_nonempty_bucket_count = nonempty_buckets;
            protocol_memory_stats_.query_route_radix_max_bucket_size = max_bucket_size;
        } else {
            protocol_query_records_.reserve(protocol_query_hashes_.size());
            for (const hash_type hash : protocol_query_hashes_) {
                protocol_query_records_.push_back(query_record{.hash = hash});
            }
        }
        protocol_ordinal_to_selected_.assign(entries.size(), no_route);
        protocol_selected_extents_.reserve(selected_count);
        for (const auto& entry : entries) {
            if (!query_intersects_entry(entry))
                continue;
            protocol_ordinal_to_selected_[entry.partition_id] = protocol_selected_extents_.size();
            protocol_selected_extents_.push_back(selected_extent{});
            protocol_max_selected_control_bytes_ =
                (std::max)(protocol_max_selected_control_bytes_, entry.control_encoded_bytes);
        }
        if constexpr (p_traits::global_controls_then_positions) {
            released_full_query_hash_bytes = bioseq::common::saturating_mul(
                static_cast<std::uint64_t>(protocol_query_hashes_.capacity()), sizeof(hash_type));
            std::vector<hash_type>{}.swap(protocol_query_hashes_);
        }
    } catch (const std::bad_alloc&) {
        capture_admission_failure_(grouped_soa_query_admission_stage::query_route_allocation,
                                   projected,
                                   0u,
                                   0u,
                                   route_bytes);
        return protocol_poison<void>(minimizer_generation_encoding_error::admission);
    }
    const std::uint64_t query_record_capacity_bytes = protocol_query_state_capacity_bytes_();
    const std::uint64_t compact_query_hash_capacity_bytes = protocol_query_hash_capacity_bytes_();
    const std::uint64_t radix_offset_capacity_bytes =
        bioseq::common::saturating_mul(protocol_query_radix_offset_count_, sizeof(std::uint32_t));
    const std::uint64_t ordinal_map_capacity_bytes = bioseq::common::saturating_mul(
        protocol_ordinal_to_selected_.capacity(), sizeof(std::size_t));
    const std::uint64_t selected_extent_vector_capacity_bytes = bioseq::common::saturating_mul(
        protocol_selected_extents_.capacity(), sizeof(selected_extent));
    if (query_record_capacity_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        compact_query_hash_capacity_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        released_full_query_hash_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        radix_offset_capacity_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        ordinal_map_capacity_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        selected_extent_vector_capacity_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        !replace_ledger_field_(protocol_owned_bytes_.query_record_bytes,
                               query_record_capacity_bytes) ||
        !replace_ledger_field_(protocol_owned_bytes_.query_hash_bytes,
                               compact_query_hash_capacity_bytes) ||
        !replace_ledger_field_(protocol_owned_bytes_.radix_offset_bytes,
                               radix_offset_capacity_bytes) ||
        !replace_ledger_field_(protocol_owned_bytes_.ordinal_map_bytes,
                               ordinal_map_capacity_bytes) ||
        !replace_ledger_field_(protocol_owned_bytes_.selected_extent_vector_bytes,
                               selected_extent_vector_capacity_bytes)) {
        return protocol_poison<void>(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    add_memory_stats_(protocol_memory_stats_.released_query_hash_capacity_bytes,
                      released_full_query_hash_bytes);
    if (inject_admission_failure_(grouped_soa_query_admission_stage::query_route_reconcile)) {
        capture_admission_failure_(grouped_soa_query_admission_stage::query_route_reconcile,
                                   projected,
                                   0u,
                                   0u,
                                   route_bytes,
                                   nullptr,
                                   true);
        return protocol_poison<void>(minimizer_generation_encoding_error::admission);
    }
    if (!route_guard.reconcile()) {
        capture_admission_failure_(grouped_soa_query_admission_stage::query_route_reconcile,
                                   projected,
                                   0u,
                                   0u,
                                   route_bytes);
        return protocol_poison<void>(minimizer_generation_encoding_error::admission);
    }
    if (!protocol_ledger_matches_oracle_()) {
        return protocol_poison<void>(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    initialize_physical_cursor();
    protocol_initialized_ = true;
    return {};
}

} // namespace minimizer_prepared_reader_detail

} // namespace bioseq::indexing
