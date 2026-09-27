/**
 * @file read_execution.cppm
 * @brief Checks index rows in storage order and makes completed query results available.
 */
export module bioseq.indexing.minimizer_hashmap:prepared_reader_core_read_path_read_execution;

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
import :prepared_reader_core_read_path_query_mapping;
import :prepared_reader_core_read_path_record_decode;

export namespace bioseq::indexing {

namespace minimizer_prepared_reader_detail {

template <class p_traits>
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
trait_driven_prepared_query_core<p_traits>::accept_protocol_row_impl(
    const minimizer_generation_row_view& row, bool row_checksum_prevalidated) {
    const auto entries = this->directory_.entries();
    if (protocol_failed_ || protocol_ready_ || protocol_entry_index_ >= entries.size()) {
        return protocol_poison<void>(minimizer_generation_encoding_error::row_order);
    }
    const grouped_soa_bucket_directory_entry& entry = entries[protocol_entry_index_];
    const bool positions = protocol_position_stage_;
    const std::size_t selected_index = entry.partition_id < protocol_ordinal_to_selected_.size()
                                           ? protocol_ordinal_to_selected_[entry.partition_id]
                                           : no_route;
    const bool selected = selected_index != no_route;
    const auto position_sub_key = derive_minimizer_position_extent_sub_key(entry.sub_key);
    if (!position_sub_key) {
        return protocol_poison<void>(position_sub_key.error());
    }
    const std::uint32_t chunk_count =
        positions ? entry.position_chunk_count : entry.control_chunk_count;
    const std::uint64_t encoded_bytes =
        positions ? entry.position_encoded_bytes : entry.control_encoded_bytes;
    if (!protocol_session_) {
        protocol_session_.emplace(extent_session{
            .entry_index = protocol_entry_index_,
            .positions = positions,
        });
        if (!positions && selected) {
            if (encoded_bytes > (std::numeric_limits<std::size_t>::max)()) {
                protocol_session_.reset();
                return protocol_poison<void>(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
            if (auto scratch = ensure_control_scratch_(); !scratch) {
                protocol_session_.reset();
                return protocol_poison<void>(scratch.error());
            }
            if (!protocol_control_scratch_.empty() ||
                protocol_control_scratch_.capacity() < static_cast<std::size_t>(encoded_bytes)) {
                protocol_session_.reset();
                return protocol_poison<void>(minimizer_generation_encoding_error::element_count);
            }
        }
    }
    extent_session& session = *protocol_session_;
    if (session.entry_index != protocol_entry_index_ || session.positions != positions ||
        session.next_chunk >= chunk_count) {
        return protocol_poison<void>(minimizer_generation_encoding_error::row_order);
    }
    constexpr std::uint32_t extent_payload_bytes = minimizer_extent_payload_bytes(p_traits::layout);
    static_assert(extent_payload_bytes != 0);
    const std::uint64_t byte_offset =
        static_cast<std::uint64_t>(session.next_chunk) * extent_payload_bytes;
    if (byte_offset >= encoded_bytes) {
        return protocol_poison<void>(minimizer_generation_encoding_error::row_order);
    }
    const std::uint64_t expected_size =
        std::min<std::uint64_t>(extent_payload_bytes, encoded_bytes - byte_offset);
    const std::uint32_t first_chunk =
        positions ? entry.position_first_chunk : entry.control_first_chunk;
    const index_sub_key_t expected_sub_key = positions ? *position_sub_key : entry.sub_key;
    if (row.address !=
            index_chunk_address{
                .generation_id = this->head_.generation_id,
                .position_segment = entry.position_segment,
                .sub_key = expected_sub_key,
                .chunk_no = first_chunk + session.next_chunk,
            } ||
        row.bytes.size() != expected_size) {
        return protocol_poison<void>(minimizer_generation_encoding_error::row_order);
    }
    if (!row_checksum_prevalidated &&
        row.row_checksum != crc64_index_chunk_row(row.address.generation_id,
                                                  row.address.position_segment,
                                                  row.address.sub_key,
                                                  row.address.chunk_no,
                                                  static_cast<std::uint32_t>(row.bytes.size()),
                                                  row.bytes)) {
        return protocol_poison<void>(minimizer_generation_encoding_error::checksum);
    }
    // The row checksum has already been checked here or by the receipt issuer.
    // Combine it into the extent checksum without scanning the payload again.
    session.crc.value =
        crc64_append_validated_index_chunk_payload(session.crc.checksum(),
                                                   row.address,
                                                   static_cast<std::uint32_t>(row.bytes.size()),
                                                   row.row_checksum);
    if (positions && selected) {
        if (auto observed = observe_position_payload(entry, row.bytes); !observed) {
            return protocol_poison<void>(observed.error());
        }
    } else if (!positions && selected) {
        if (protocol_control_scratch_.size() > static_cast<std::size_t>(encoded_bytes) ||
            row.bytes.size() >
                protocol_control_scratch_.capacity() - protocol_control_scratch_.size() ||
            row.bytes.size() >
                static_cast<std::size_t>(encoded_bytes) - protocol_control_scratch_.size()) {
            return protocol_poison<void>(minimizer_generation_encoding_error::element_count);
        }
        protocol_control_scratch_.insert(
            protocol_control_scratch_.end(), row.bytes.begin(), row.bytes.end());
    }
    ++session.next_chunk;
    if (session.next_chunk != chunk_count)
        return {};
    const std::uint64_t expected_crc = positions ? entry.position_checksum : entry.control_checksum;
    if (session.crc.checksum() != expected_crc) {
        return protocol_poison<void>(minimizer_generation_encoding_error::checksum);
    }
    if (!positions && selected) {
        const auto observed = observe_control_extent(entry, protocol_control_scratch_);
        if (!observed) {
            return protocol_poison<void>(observed.error());
        }
        protocol_control_scratch_.clear();
    } else if (positions && selected) {
        selected_extent& extent = protocol_selected_extents_[selected_index];
        while (extent.next_control < selected_control_count_(extent) &&
               extent.next_position >= selected_position_end_(extent, extent.next_control)) {
            if constexpr (p_traits::global_controls_then_positions) {
                auto& routes = extent.route_state;
                if (routes.uses_sparse && routes.next_sparse_route < routes.sparse_route_count) {
                    const std::uint32_t route_ordinal =
                        routes.sparse_routes[routes.next_sparse_route].control_ordinal;
                    if (route_ordinal < extent.next_control) {
                        return protocol_poison<void>(
                            minimizer_generation_encoding_error::element_count);
                    }
                    if (route_ordinal == extent.next_control) {
                        ++routes.next_sparse_route;
                    }
                }
            }
            ++extent.next_control;
            extent.previous_position_valid = false;
        }
        bool routes_complete = true;
        if constexpr (p_traits::global_controls_then_positions) {
            const auto& routes = extent.route_state;
            routes_complete =
                routes.uses_sparse
                    ? !routes.dense_routes && routes.next_sparse_route == routes.sparse_route_count
                    : !routes.sparse_routes &&
                          (selected_control_count_(extent) == 0u || routes.dense_routes != nullptr);
        }
        if (!extent.controls_complete || extent.next_position != entry.position_element_count ||
            extent.next_control != selected_control_count_(extent) || !routes_complete) {
            return protocol_poison<void>(minimizer_generation_encoding_error::element_count);
        }
        extent.positions_complete = true;
        if constexpr (p_traits::global_controls_then_positions) {
            extent.control_state.position_ends.reset();
            extent.control_state.control_count = 0u;
            extent.route_state.dense_routes.reset();
            extent.route_state.sparse_routes.reset();
            extent.route_state.sparse_route_count = 0u;
            extent.route_state.next_sparse_route = 0u;
            extent.route_state.uses_sparse = false;
        } else {
            std::vector<selected_control>{}.swap(extent.control_state.controls);
        }
        if (!replace_selected_extent_owner_(extent, 0u)) {
            return protocol_poison<void>(minimizer_generation_encoding_error::arithmetic_overflow);
        }
    }
    protocol_session_.reset();
    advance_physical_cursor();
    if constexpr (p_traits::global_controls_then_positions) {
        if (protocol_position_stage_ && !protocol_global_position_storage_prepared_) {
            if (!release_control_scratch_noexcept_()) {
                return protocol_poison<void>(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
            this->permit_.shrink_to(protocol_owned_capacity_bytes());
            if (!protocol_ledger_matches_oracle_()) {
                return protocol_poison<void>(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
            if (auto prepared = prepare_global_position_storage(); !prepared) {
                return protocol_poison<void>(prepared.error());
            }
        }
    }
    this->permit_.shrink_to(protocol_owned_capacity_bytes());
    return {};
}

template <class p_traits>
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
trait_driven_prepared_query_core<p_traits>::accept_protocol_row(
    const minimizer_generation_row_view& row) {
    return accept_protocol_row_impl(row, false);
}

template <class p_traits>
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
trait_driven_prepared_query_core<p_traits>::accept_protocol_row(
    const validated_minimizer_generation_row_receipt& receipt) {
    if (!receipt.has_valid_authority()) {
        return protocol_poison<void>(minimizer_generation_encoding_error::checksum);
    }
    return accept_protocol_row_impl(
        minimizer_generation_row_view{
            .address = receipt.address(),
            .bytes = receipt.bytes(),
            .row_checksum = receipt.row_checksum(),
        },
        true);
}

template <class p_traits>
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
trait_driven_prepared_query_core<p_traits>::finish_query_protocol() {
    if (protocol_failed_ || protocol_ready_ || protocol_session_ ||
        protocol_entry_index_ != this->directory_.entries().size() || protocol_position_stage_) {
        return protocol_poison<void>(minimizer_generation_encoding_error::row_count);
    }
    for (const selected_extent& extent : protocol_selected_extents_) {
        if (!extent.controls_complete || !extent.positions_complete) {
            return protocol_poison<void>(minimizer_generation_encoding_error::row_count);
        }
    }
    if constexpr (p_traits::global_controls_then_positions) {
        for (std::size_t query_index = 0u; query_index < protocol_query_count_; ++query_index) {
            const std::uint64_t exact_count = protocol_query_exact_count_(query_index);
            const bool retained = protocol_query_retained_(query_index);
            const std::uint64_t written = protocol_query_written_.get(query_index);
            if (exact_count == 0u || exact_count > protocol_max_occ_) {
                if (retained || written != 0u) {
                    return protocol_poison<void>(
                        minimizer_generation_encoding_error::element_count);
                }
                continue;
            }
            const std::uint64_t position_offset = protocol_query_position_offsets_[query_index];
            if (!retained || written != exact_count ||
                position_offset > protocol_global_position_count_ ||
                exact_count > protocol_global_position_count_ - position_offset) {
                return protocol_poison<void>(minimizer_generation_encoding_error::element_count);
            }
        }
        protocol_query_written_.reset();
        if (!replace_ledger_field_(protocol_owned_bytes_.query_record_bytes,
                                   protocol_query_state_capacity_bytes_())) {
            return protocol_poison<void>(minimizer_generation_encoding_error::arithmetic_overflow);
        }
    } else {
        for (const query_record& record : protocol_query_records_) {
            if (record.positions_complete &&
                record.positions.size() != record.global_occurrence_count) {
                return protocol_poison<void>(minimizer_generation_encoding_error::element_count);
            }
        }
    }
    if (!release_control_scratch_noexcept_()) {
        return protocol_poison<void>(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    this->permit_.shrink_to(protocol_owned_capacity_bytes());
    if (!protocol_ledger_matches_oracle_()) {
        return protocol_poison<void>(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    protocol_ready_ = true;
    return {};
}

template <class p_traits>
[[nodiscard]] inline auto
trait_driven_prepared_query_core<p_traits>::lookup_protocol(std::uint64_t hash) const noexcept
    -> protocol_lookup {
    if (!protocol_ready_ || hash > (std::numeric_limits<hash_type>::max)())
        return {};
    const std::size_t query_index = query_index_for_hash(hash);
    if (query_index == no_route || query_index >= protocol_query_state_count_())
        return {};
    std::span<const std::uint64_t> positions;
    if constexpr (p_traits::global_controls_then_positions) {
        const std::uint64_t exact_count = protocol_query_exact_count_(query_index);
        const bool retained = protocol_query_retained_(query_index);
        const bool positions_complete = exact_count == 0u || retained;
        if (retained && exact_count != 0u &&
            protocol_query_position_offsets_[query_index] <= protocol_global_position_count_ &&
            exact_count <=
                protocol_global_position_count_ - protocol_query_position_offsets_[query_index]) {
            positions = std::span<const std::uint64_t>{
                protocol_global_positions_.get() +
                    static_cast<std::size_t>(protocol_query_position_offsets_[query_index]),
                static_cast<std::size_t>(exact_count)};
        }
        return protocol_lookup{
            .global_occurrence_count = exact_count,
            .positions_complete = positions_complete,
            .positions = positions,
        };
    } else {
        const query_record& found = protocol_query_records_[query_index];
        positions = found.positions;
        return protocol_lookup{
            .global_occurrence_count = found.global_occurrence_count,
            .positions_complete = found.positions_complete,
            .positions = positions,
        };
    }
}

template <class p_traits>
[[nodiscard]] inline std::size_t
trait_driven_prepared_query_core<p_traits>::protocol_retained_position_count() const noexcept {
    if constexpr (p_traits::global_controls_then_positions) {
        return protocol_global_position_count_;
    } else {
        std::size_t count = 0;
        for (const query_record& record : protocol_query_records_) {
            if (record.positions.size() > (std::numeric_limits<std::size_t>::max)() - count) {
                return (std::numeric_limits<std::size_t>::max)();
            }
            count += record.positions.size();
        }
        return count;
    }
}

template <class p_traits>
[[nodiscard]] inline const grouped_soa_query_admission_stats&
trait_driven_prepared_query_core<p_traits>::protocol_admission_failure_stats() const noexcept {
    return protocol_admission_failure_stats_;
}

} // namespace minimizer_prepared_reader_detail

} // namespace bioseq::indexing
