/**
 * @file admission.cppm
 * @brief Implements admitted position and control-buffer ownership.
 */
export module bioseq.indexing.minimizer_hashmap:prepared_reader_core_read_path_admission;

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

export namespace bioseq::indexing {

namespace minimizer_prepared_reader_detail {

template <class p_traits>
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
trait_driven_prepared_query_core<p_traits>::prepare_global_position_storage() {
    if constexpr (!p_traits::global_controls_then_positions) {
        return {};
    }
    if (protocol_global_position_storage_prepared_ || !protocol_position_stage_ ||
        protocol_entry_index_ != 0u || protocol_session_ || protocol_global_positions_ ||
        protocol_global_position_count_ != 0u || protocol_query_position_offsets_ ||
        protocol_query_position_offset_count_ != 0u || protocol_query_written_.count != 0u) {
        return std::unexpected(minimizer_generation_encoding_error::row_order);
    }
    increment_memory_stats_(protocol_memory_stats_.global_position_storage_preparations);

    std::uint64_t retained_count = 0;
    for (std::size_t query_index = 0u; query_index < protocol_query_count_; ++query_index) {
        const std::uint64_t exact_count = protocol_query_exact_count_(query_index);
        if (exact_count > this->manifest_.position_count) {
            return std::unexpected(minimizer_generation_encoding_error::element_count);
        }
        if (exact_count == 0u || exact_count > protocol_max_occ_) {
            continue;
        }
        if (exact_count > (std::numeric_limits<std::uint64_t>::max)() - retained_count) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        retained_count += exact_count;
    }
    if (retained_count > (std::numeric_limits<std::size_t>::max)() / sizeof(std::uint64_t)) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    permit_growth_guard guard{*this};
    const auto planned =
        plan_grouped_soa_position_image(retained_count, protocol_owned_capacity_bytes());
    if (!planned)
        return std::unexpected(planned.error());
    const std::uint64_t retained_bytes = planned->retained_position_bytes;
    const std::uint64_t headroom = planned->allocation_headroom_bytes;
    const prepared_query_scalar_width written_width =
        prepared_query_scalar_width_for_max(protocol_max_occ_);
    using bioseq::common::saturating_add;
    using bioseq::common::saturating_mul;
    std::uint64_t compact_final_bytes =
        saturating_mul(static_cast<std::uint64_t>(protocol_query_count_), sizeof(std::uint64_t));
    compact_final_bytes =
        saturating_add(compact_final_bytes,
                       saturating_mul(static_cast<std::uint64_t>(protocol_query_count_),
                                      static_cast<std::uint8_t>(written_width)));
    const std::uint64_t projected = saturating_add(planned->projected_bytes, compact_final_bytes);
    if (compact_final_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        projected == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    if (inject_admission_failure_(grouped_soa_query_admission_stage::position_image_reservation)) {
        capture_admission_failure_(grouped_soa_query_admission_stage::position_image_reservation,
                                   projected,
                                   retained_count,
                                   retained_bytes,
                                   headroom,
                                   nullptr,
                                   true);
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }
    bioseq::parallelism::memory_admission_failure position_failure;
    if (!guard.grow(projected, &position_failure)) {
        capture_admission_failure_(grouped_soa_query_admission_stage::position_image_reservation,
                                   projected,
                                   retained_count,
                                   retained_bytes,
                                   headroom,
                                   &position_failure);
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }
    if (retained_count != 0u &&
        inject_admission_failure_(grouped_soa_query_admission_stage::position_image_allocation)) {
        capture_admission_failure_(grouped_soa_query_admission_stage::position_image_allocation,
                                   projected,
                                   retained_count,
                                   retained_bytes,
                                   headroom,
                                   nullptr,
                                   true);
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }
    try {
        std::unique_ptr<std::uint64_t[]> position_offsets;
        prepared_query_scalar_array written;
        std::unique_ptr<std::uint64_t[]> global_positions;
        if (protocol_query_count_ != 0u) {
            position_offsets =
                std::make_unique_for_overwrite<std::uint64_t[]>(protocol_query_count_);
            std::fill_n(position_offsets.get(),
                        protocol_query_count_,
                        (std::numeric_limits<std::uint64_t>::max)());
            written.allocate(written_width, protocol_query_count_, true);
        }
        if (retained_count != 0u) {
            global_positions = std::make_unique_for_overwrite<std::uint64_t[]>(
                static_cast<std::size_t>(retained_count));
        }
        std::uint64_t retained_offset = 0u;
        for (std::size_t query_index = 0u; query_index < protocol_query_count_; ++query_index) {
            const std::uint64_t exact_count = protocol_query_exact_count_(query_index);
            if (exact_count == 0u || exact_count > protocol_max_occ_) {
                continue;
            }
            position_offsets[query_index] = retained_offset;
            retained_offset += exact_count;
        }
        if (retained_offset != retained_count) {
            return std::unexpected(minimizer_generation_encoding_error::element_count);
        }
        protocol_query_position_offsets_ = std::move(position_offsets);
        protocol_query_position_offset_count_ = protocol_query_count_;
        protocol_query_written_ = std::move(written);
        protocol_global_positions_ = std::move(global_positions);
        protocol_global_position_count_ = static_cast<std::size_t>(retained_count);
    } catch (const std::bad_alloc&) {
        capture_admission_failure_(grouped_soa_query_admission_stage::position_image_allocation,
                                   projected,
                                   retained_count,
                                   retained_bytes,
                                   headroom);
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }
    if (!replace_ledger_field_(protocol_owned_bytes_.global_position_bytes, retained_bytes) ||
        !replace_ledger_field_(protocol_owned_bytes_.query_record_bytes,
                               protocol_query_state_capacity_bytes_())) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    if (retained_count != 0u) {
        increment_memory_stats_(protocol_memory_stats_.global_position_storage_allocations);
    }
    if (inject_admission_failure_(grouped_soa_query_admission_stage::position_image_reconcile)) {
        capture_admission_failure_(grouped_soa_query_admission_stage::position_image_reconcile,
                                   projected,
                                   retained_count,
                                   retained_bytes,
                                   headroom,
                                   nullptr,
                                   true);
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }
    if (!guard.reconcile()) {
        capture_admission_failure_(grouped_soa_query_admission_stage::position_image_reconcile,
                                   projected,
                                   retained_count,
                                   retained_bytes,
                                   headroom);
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }
    if (!protocol_ledger_matches_oracle_()) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    protocol_global_position_storage_prepared_ = true;
    return {};
}

template <class p_traits>
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
trait_driven_prepared_query_core<p_traits>::ensure_control_scratch_() {
    if (!protocol_control_scratch_.empty()) {
        return std::unexpected(minimizer_generation_encoding_error::row_order);
    }
    if (protocol_owned_bytes_.control_scratch_bytes != 0u)
        return {};
    if (protocol_max_selected_control_bytes_ > (std::numeric_limits<std::size_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    const std::uint64_t headroom =
        bioseq::common::saturating_mul(protocol_max_selected_control_bytes_,
                                       bioseq::parallelism::k_vector_preallocation_headroom_factor);
    const std::uint64_t projected =
        bioseq::common::saturating_add(protocol_owned_capacity_bytes(), headroom);
    if (headroom == (std::numeric_limits<std::uint64_t>::max)() ||
        projected == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    permit_growth_guard guard{*this};
    if (inject_admission_failure_(grouped_soa_query_admission_stage::control_payload_reservation)) {
        capture_admission_failure_(grouped_soa_query_admission_stage::control_payload_reservation,
                                   projected,
                                   0u,
                                   0u,
                                   headroom,
                                   nullptr,
                                   true);
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }
    bioseq::parallelism::memory_admission_failure payload_failure;
    if (!guard.grow(projected, &payload_failure)) {
        capture_admission_failure_(grouped_soa_query_admission_stage::control_payload_reservation,
                                   projected,
                                   0u,
                                   0u,
                                   headroom,
                                   &payload_failure);
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }
    if (inject_admission_failure_(grouped_soa_query_admission_stage::control_payload_allocation)) {
        capture_admission_failure_(grouped_soa_query_admission_stage::control_payload_allocation,
                                   projected,
                                   0u,
                                   0u,
                                   headroom,
                                   nullptr,
                                   true);
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }
    try {
        protocol_control_scratch_.reserve(
            static_cast<std::size_t>(protocol_max_selected_control_bytes_));
    } catch (const std::bad_alloc&) {
        capture_admission_failure_(grouped_soa_query_admission_stage::control_payload_allocation,
                                   projected,
                                   0u,
                                   0u,
                                   headroom);
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }
    const std::uint64_t actual_bytes = protocol_control_scratch_.capacity();
    if (!replace_ledger_field_(protocol_owned_bytes_.control_scratch_bytes, actual_bytes)) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    if (inject_admission_failure_(grouped_soa_query_admission_stage::control_payload_reconcile)) {
        capture_admission_failure_(grouped_soa_query_admission_stage::control_payload_reconcile,
                                   projected,
                                   0u,
                                   0u,
                                   headroom,
                                   nullptr,
                                   true);
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }
    if (!guard.reconcile()) {
        capture_admission_failure_(grouped_soa_query_admission_stage::control_payload_reconcile,
                                   projected,
                                   0u,
                                   0u,
                                   headroom);
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }
    return {};
}

} // namespace minimizer_prepared_reader_detail

} // namespace bioseq::indexing
