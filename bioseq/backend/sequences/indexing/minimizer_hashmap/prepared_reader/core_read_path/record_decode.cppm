/**
 * @file record_decode.cppm
 * @brief Implements authenticated control and position record decoding.
 */
export module bioseq.indexing.minimizer_hashmap:prepared_reader_core_read_path_record_decode;

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

export namespace bioseq::indexing {

namespace minimizer_prepared_reader_detail {

template <class p_traits>
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
trait_driven_prepared_query_core<p_traits>::observe_control_extent(
    const grouped_soa_bucket_directory_entry& entry, std::span<const std::byte> bytes) {
    const std::size_t selected_index = entry.partition_id < protocol_ordinal_to_selected_.size()
                                           ? protocol_ordinal_to_selected_[entry.partition_id]
                                           : no_route;
    std::uint64_t transient = bioseq::common::saturating_mul(
        entry.control_element_count, sizeof(typename wire_ops::control_type));
    // Keep every boundary in an intersected extent for strict per-control
    // position-order validation. grouped-SoA stores query routes sparsely.
    // admit the conservative all-routes peak before the one decode pass.
    const std::uint64_t retained_control_count =
        selected_index == no_route ? 0u : entry.control_element_count;
    const std::uint64_t retained_control_width =
        p_traits::global_controls_then_positions
            ? static_cast<std::uint8_t>(
                  prepared_query_scalar_width_for_max(entry.position_element_count))
            : sizeof(selected_control);
    std::uint64_t retained =
        bioseq::common::saturating_mul(retained_control_count, retained_control_width);
    if constexpr (p_traits::global_controls_then_positions) {
        // One dense u32 route image is always built.  Sparse conversion
        // happens only below 50% density, so its peak is bounded by one
        // additional dense-image worth of bytes.
        const std::uint64_t dense_route_bytes =
            bioseq::common::saturating_mul(retained_control_count, sizeof(std::uint32_t));
        retained = bioseq::common::saturating_add(retained, dense_route_bytes);
        retained = bioseq::common::saturating_add(retained, dense_route_bytes);
    }
    const std::uint64_t additional = bioseq::common::saturating_add(transient, retained);
    const std::uint64_t transient_headroom = bioseq::common::saturating_mul(
        transient, bioseq::parallelism::k_vector_preallocation_headroom_factor);
    /// Select exact-array or vector-growth headroom for retained control state.
    const std::uint64_t retained_headroom = [&retained]() noexcept {
        if constexpr (p_traits::global_controls_then_positions) {
            // grouped-SoA ends and route images are exact arrays. Their
            // admitted peak is already represented by retained and must
            // not receive vector growth headroom a second time.
            return retained;
        } else {
            return bioseq::common::saturating_mul(
                retained, bioseq::parallelism::k_vector_preallocation_headroom_factor);
        }
    }();
    const std::uint64_t headroom =
        bioseq::common::saturating_add(transient_headroom, retained_headroom);
    permit_growth_guard guard{*this};
    const std::uint64_t projected =
        bioseq::common::saturating_add(protocol_owned_capacity_bytes(), headroom);
    if (additional == (std::numeric_limits<std::uint64_t>::max)() ||
        headroom == (std::numeric_limits<std::uint64_t>::max)() ||
        projected == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    if (inject_admission_failure_(grouped_soa_query_admission_stage::control_decode_reservation)) {
        capture_admission_failure_(grouped_soa_query_admission_stage::control_decode_reservation,
                                   projected,
                                   0u,
                                   0u,
                                   headroom,
                                   nullptr,
                                   true);
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }
    bioseq::parallelism::memory_admission_failure control_failure;
    if (!guard.grow(projected, &control_failure)) {
        capture_admission_failure_(grouped_soa_query_admission_stage::control_decode_reservation,
                                   projected,
                                   0u,
                                   0u,
                                   headroom,
                                   &control_failure);
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }
    if (inject_admission_failure_(grouped_soa_query_admission_stage::control_decode_allocation)) {
        capture_admission_failure_(grouped_soa_query_admission_stage::control_decode_allocation,
                                   projected,
                                   0u,
                                   0u,
                                   headroom,
                                   nullptr,
                                   true);
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }
    /// Decode controls and publish their query routes as one allocation transaction.
    const auto decoded_result = [this, &bytes, &entry, &selected_index, &projected, &headroom]()
        -> std::expected<void, minimizer_generation_encoding_error> {
        try {
            const auto decoded = wire_ops::decode_controls(bytes, entry, this->profile_);
            if (!decoded)
                return std::unexpected(decoded.error());
            add_memory_stats_(protocol_memory_stats_.decoded_control_records,
                              static_cast<std::uint64_t>(decoded->size()));
            if (selected_index == no_route)
                return {};
            selected_extent& selected = protocol_selected_extents_[selected_index];
            const std::size_t retained_before = selected_control_count_(selected);
            if (retained_before != 0u || selected.controls_complete) {
                return std::unexpected(minimizer_generation_encoding_error::row_order);
            }
            if constexpr (p_traits::global_controls_then_positions) {
                prepared_query_scalar_array position_ends;
                std::unique_ptr<std::uint32_t[]> dense_routes;
                if (decoded->size() != 0u) {
                    position_ends.allocate(
                        prepared_query_scalar_width_for_max(entry.position_element_count),
                        decoded->size(),
                        false);
                    dense_routes = std::make_unique_for_overwrite<std::uint32_t[]>(decoded->size());
                    std::fill_n(dense_routes.get(), decoded->size(), no_narrow_route);
                }
                // Publish both exact owners together only after every
                // allocation succeeds, keeping failure stats exact.
                selected.control_state.position_ends = std::move(position_ends);
                selected.route_state.dense_routes = std::move(dense_routes);
                selected.control_state.control_count = decoded->size();
            } else {
                selected.control_state.controls.reserve(decoded->size());
            }
            std::uint64_t expected_position_begin = 0u;
            std::size_t control_ordinal = 0u;
            std::size_t routed_control_count = 0u;
            std::uint64_t route_hash_probes = 0u;
            if constexpr (p_traits::global_controls_then_positions) {
                constexpr std::uint64_t max_probes_per_lookup =
                    std::numeric_limits<std::uint32_t>::digits + 1u;
                if (decoded->size() >
                    (std::numeric_limits<std::uint64_t>::max)() / max_probes_per_lookup) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::arithmetic_overflow);
                }
            }
            for (const auto& control : *decoded) {
                std::uint32_t control_hash_probes = 0u;
                const std::size_t query_index =
                    query_index_for_control_hash(wire_ops::hash(control), control_hash_probes);
                route_hash_probes += control_hash_probes;
                const std::uint64_t begin = wire_ops::position_begin(control);
                const std::uint64_t end = wire_ops::position_end(control);
                // Both wire codecs already enforce this partition while
                // decoding.  Recheck it at the retained-image boundary so
                // position_begin does not need to survive per control.
                if (begin != expected_position_begin || end <= begin) {
                    return std::unexpected(minimizer_generation_encoding_error::control_range);
                }
                expected_position_begin = end;
                if (query_index != no_route) {
                    const std::uint64_t count = end - begin;
                    if constexpr (p_traits::global_controls_then_positions) {
                        if (auto added = add_protocol_query_exact_count_(query_index, count);
                            !added) {
                            return added;
                        }
                    } else {
                        query_record& record = protocol_query_records_[query_index];
                        if (record.global_occurrence_count >
                            (std::numeric_limits<std::uint64_t>::max)() - count) {
                            return std::unexpected(
                                minimizer_generation_encoding_error::arithmetic_overflow);
                        }
                        record.global_occurrence_count += count;
                        if (record.global_occurrence_count > protocol_max_occ_) {
                            const std::uint64_t old_position_bytes = bioseq::common::saturating_mul(
                                record.positions.capacity(), sizeof(std::uint64_t));
                            std::vector<std::uint64_t>{}.swap(record.positions);
                            if (old_position_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                                old_position_bytes >
                                    protocol_owned_bytes_.query_record_position_bytes ||
                                !replace_ledger_field_(
                                    protocol_owned_bytes_.query_record_position_bytes,
                                    protocol_owned_bytes_.query_record_position_bytes -
                                        old_position_bytes)) {
                                return std::unexpected(
                                    minimizer_generation_encoding_error::arithmetic_overflow);
                            }
                            record.positions_complete = false;
                        }
                    }
                }
                if constexpr (p_traits::global_controls_then_positions) {
                    if (!selected.control_state.position_ends.set(control_ordinal, end)) {
                        return std::unexpected(minimizer_generation_encoding_error::control_range);
                    }
                    if (query_index != no_route) {
                        if (query_index >= no_narrow_route) {
                            return std::unexpected(
                                minimizer_generation_encoding_error::arithmetic_overflow);
                        }
                        selected.route_state.dense_routes[control_ordinal] =
                            static_cast<std::uint32_t>(query_index);
                        ++routed_control_count;
                    }
                } else {
                    selected.control_state.controls.push_back(selected_control{
                        .query_index = query_index,
                        .position_end = end,
                    });
                }
                ++control_ordinal;
            }
            if (expected_position_begin != entry.position_element_count) {
                return std::unexpected(minimizer_generation_encoding_error::control_range);
            }
            if constexpr (p_traits::global_controls_then_positions) {
                const std::size_t control_count = selected.control_state.control_count;
                if (control_count <= no_narrow_route &&
                    routed_control_count < control_count - routed_control_count) {
                    if (routed_control_count != 0u) {
                        selected.route_state.sparse_routes =
                            std::make_unique_for_overwrite<sparse_selected_route[]>(
                                routed_control_count);
                    }
                    std::size_t sparse_index = 0u;
                    for (std::size_t ordinal = 0u; ordinal < control_count; ++ordinal) {
                        const std::uint32_t query_route =
                            selected.route_state.dense_routes[ordinal];
                        if (query_route == no_narrow_route)
                            continue;
                        selected.route_state.sparse_routes[sparse_index++] = sparse_selected_route{
                            .control_ordinal = static_cast<std::uint32_t>(ordinal),
                            .query_index = query_route,
                        };
                    }
                    if (sparse_index != routed_control_count) {
                        return std::unexpected(minimizer_generation_encoding_error::element_count);
                    }
                    selected.route_state.sparse_route_count = routed_control_count;
                    selected.route_state.dense_routes.reset();
                    selected.route_state.uses_sparse = true;
                }
                // Update stats once per decoded extent.  A counter write
                // on every binary-search comparison would itself become
                // material at hundreds of millions of controls.
                add_memory_stats_(protocol_memory_stats_.query_route_control_lookups,
                                  static_cast<std::uint64_t>(decoded->size()));
                add_memory_stats_(protocol_memory_stats_.query_route_hash_probes,
                                  route_hash_probes);
                add_memory_stats_(protocol_memory_stats_.query_route_matches,
                                  static_cast<std::uint64_t>(routed_control_count));
            }
            add_memory_stats_(
                protocol_memory_stats_.retained_selected_control_records,
                static_cast<std::uint64_t>(selected_control_count_(selected) - retained_before));
            add_memory_stats_(protocol_memory_stats_.retained_selected_control_capacity_bytes,
                              selected_extent_control_capacity_bytes_(selected));
            if (!replace_selected_extent_owner_(
                    selected, selected_extent_control_capacity_bytes_(selected))) {
                return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
            }
            selected.controls_complete = true;
            return {};
        } catch (const std::bad_alloc&) {
            capture_admission_failure_(grouped_soa_query_admission_stage::control_decode_allocation,
                                       projected,
                                       0u,
                                       0u,
                                       headroom);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
    }();
    if (!decoded_result) {
        return std::unexpected(decoded_result.error());
    }
    if (inject_admission_failure_(grouped_soa_query_admission_stage::control_decode_reconcile)) {
        capture_admission_failure_(grouped_soa_query_admission_stage::control_decode_reconcile,
                                   projected,
                                   0u,
                                   0u,
                                   headroom,
                                   nullptr,
                                   true);
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }
    if (!guard.reconcile()) {
        capture_admission_failure_(grouped_soa_query_admission_stage::control_decode_reconcile,
                                   projected,
                                   0u,
                                   0u,
                                   headroom);
        return std::unexpected(minimizer_generation_encoding_error::admission);
    }
    return {};
}

template <class p_traits>
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
trait_driven_prepared_query_core<p_traits>::append_retained_position(std::size_t query_index,
                                                                     std::uint64_t value) {
    if constexpr (p_traits::global_controls_then_positions) {
        if (query_index >= protocol_query_count_) {
            return std::unexpected(minimizer_generation_encoding_error::element_count);
        }
        const std::uint64_t exact_count = protocol_query_exact_count_(query_index);
        if (exact_count == 0u || exact_count > protocol_max_occ_) {
            return {};
        }
        const std::uint64_t position_offset = protocol_query_retained_(query_index)
                                                  ? protocol_query_position_offsets_[query_index]
                                                  : (std::numeric_limits<std::uint64_t>::max)();
        const std::uint64_t positions_written = protocol_query_written_.get(query_index);
        if (!protocol_global_position_storage_prepared_ || !protocol_query_retained_(query_index) ||
            positions_written >= exact_count || position_offset > protocol_global_position_count_ ||
            positions_written > protocol_global_position_count_ - position_offset) {
            return std::unexpected(minimizer_generation_encoding_error::element_count);
        }
        const std::uint64_t destination = position_offset + positions_written;
        if (destination >= protocol_global_position_count_) {
            return std::unexpected(minimizer_generation_encoding_error::element_count);
        }
        if (positions_written != 0u &&
            value <= protocol_global_positions_[static_cast<std::size_t>(destination - 1u)]) {
            return std::unexpected(minimizer_generation_encoding_error::position_range);
        }
        protocol_global_positions_[static_cast<std::size_t>(destination)] = value;
        if (!protocol_query_written_.increment(query_index)) {
            return std::unexpected(minimizer_generation_encoding_error::element_count);
        }
        return {};
    } else {
        if (query_index >= protocol_query_records_.size()) {
            return std::unexpected(minimizer_generation_encoding_error::element_count);
        }
        query_record& record = protocol_query_records_[query_index];
        if (!record.positions_complete)
            return {};
        if (!record.positions.empty() && value <= record.positions.back()) {
            return std::unexpected(minimizer_generation_encoding_error::position_range);
        }
        if (record.positions.size() == record.positions.capacity()) {
            increment_memory_stats_(protocol_memory_stats_.position_append_growth_reservations);
            const std::size_t maximum = static_cast<std::size_t>(
                std::min<std::uint64_t>(protocol_max_occ_, record.positions.max_size()));
            const std::size_t required = record.positions.size() + 1u;
            if (required > maximum) {
                return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
            }
            std::size_t wanted = record.positions.empty() ? std::min<std::size_t>(maximum, 8u)
                                 : record.positions.capacity() > maximum / 2u
                                     ? maximum
                                     : record.positions.capacity() * 2u;
            wanted = std::max(wanted, required);
            permit_growth_guard guard{*this};
            const std::uint64_t old_position_bytes =
                bioseq::common::saturating_mul(record.positions.capacity(), sizeof(std::uint64_t));
            const std::uint64_t additional =
                bioseq::common::saturating_mul(wanted, sizeof(std::uint64_t));
            const std::uint64_t projected = bioseq::common::saturating_add(
                protocol_owned_capacity_bytes(),
                bioseq::common::saturating_mul(
                    additional, bioseq::parallelism::k_vector_preallocation_headroom_factor));
            if (!guard.grow(projected)) {
                return std::unexpected(minimizer_generation_encoding_error::admission);
            }
            try {
                record.positions.reserve(wanted);
            } catch (const std::bad_alloc&) {
                return std::unexpected(minimizer_generation_encoding_error::admission);
            }
            const std::uint64_t new_position_bytes =
                bioseq::common::saturating_mul(record.positions.capacity(), sizeof(std::uint64_t));
            if (old_position_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                new_position_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                old_position_bytes > protocol_owned_bytes_.query_record_position_bytes) {
                return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
            }
            const std::uint64_t position_owner_without_old =
                protocol_owned_bytes_.query_record_position_bytes - old_position_bytes;
            if (new_position_bytes >
                    (std::numeric_limits<std::uint64_t>::max)() - position_owner_without_old ||
                !replace_ledger_field_(protocol_owned_bytes_.query_record_position_bytes,
                                       position_owner_without_old + new_position_bytes)) {
                return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
            }
            if (!guard.reconcile()) {
                return std::unexpected(minimizer_generation_encoding_error::admission);
            }
        }
        record.positions.push_back(value);
        return {};
    }
}

template <class p_traits>
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
trait_driven_prepared_query_core<p_traits>::observe_position_payload(
    const grouped_soa_bucket_directory_entry& entry, std::span<const std::byte> payload) {
    const std::size_t width = static_cast<std::size_t>(this->profile_.position_width);
    if (width == 0u || payload.size() % width != 0u) {
        return std::unexpected(minimizer_generation_encoding_error::byte_count);
    }
    const std::size_t selected_index = entry.partition_id < protocol_ordinal_to_selected_.size()
                                           ? protocol_ordinal_to_selected_[entry.partition_id]
                                           : no_route;
    selected_extent* selected =
        selected_index == no_route ? nullptr : &protocol_selected_extents_[selected_index];
    const auto core = derive_minimizer_position_segment_bounds(
        this->manifest_, static_cast<std::uint32_t>(entry.position_segment));
    if (!core)
        return std::unexpected(core.error());
    for (std::size_t offset = 0; offset < payload.size(); offset += width) {
        const auto decoded =
            wire_ops::decode_position(payload.subspan(offset, width), *core, this->profile_);
        if (!decoded)
            return std::unexpected(decoded.error());
        if (selected != nullptr) {
            while (selected->next_control < selected_control_count_(*selected) &&
                   selected->next_position >=
                       selected_position_end_(*selected, selected->next_control)) {
                if constexpr (p_traits::global_controls_then_positions) {
                    auto& routes = selected->route_state;
                    if (routes.uses_sparse &&
                        routes.next_sparse_route < routes.sparse_route_count) {
                        const std::uint32_t route_ordinal =
                            routes.sparse_routes[routes.next_sparse_route].control_ordinal;
                        if (route_ordinal < selected->next_control) {
                            return std::unexpected(
                                minimizer_generation_encoding_error::element_count);
                        }
                        if (route_ordinal == selected->next_control) {
                            ++routes.next_sparse_route;
                        }
                    }
                }
                ++selected->next_control;
                selected->previous_position_valid = false;
            }
            if (selected->next_control < selected_control_count_(*selected)) {
                std::size_t query_index = no_route;
                if constexpr (p_traits::global_controls_then_positions) {
                    const auto& routes = selected->route_state;
                    if (routes.uses_sparse) {
                        if (routes.next_sparse_route < routes.sparse_route_count) {
                            const sparse_selected_route& route =
                                routes.sparse_routes[routes.next_sparse_route];
                            if (route.control_ordinal < selected->next_control) {
                                return std::unexpected(
                                    minimizer_generation_encoding_error::element_count);
                            }
                            if (route.control_ordinal == selected->next_control) {
                                query_index = route.query_index;
                            }
                        }
                    } else if (!routes.dense_routes) {
                        return std::unexpected(minimizer_generation_encoding_error::element_count);
                    } else {
                        const std::uint32_t route = routes.dense_routes[selected->next_control];
                        if (route != no_narrow_route)
                            query_index = route;
                    }
                } else {
                    query_index =
                        selected->control_state.controls[selected->next_control].query_index;
                }
                if (selected->previous_position_valid && *decoded <= selected->previous_position) {
                    return std::unexpected(minimizer_generation_encoding_error::position_range);
                }
                selected->previous_position = *decoded;
                selected->previous_position_valid = true;
                if (query_index != no_route) {
                    if (auto appended = append_retained_position(query_index, *decoded);
                        !appended) {
                        return appended;
                    }
                }
            }
            ++selected->next_position;
        }
    }
    return {};
}

} // namespace minimizer_prepared_reader_detail

} // namespace bioseq::indexing
