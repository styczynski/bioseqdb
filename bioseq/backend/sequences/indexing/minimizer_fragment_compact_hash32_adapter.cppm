/**
 * @file minimizer_fragment_compact_hash32_adapter.cppm
 * @brief Replayable grouped-SoA minimizer generation to compact exact hash32 run adapter.
 *
 * The adapter borrows an already-authenticated manifest, decoded directory,
 * and mapped payload.  It never owns a target-wide control or position image.
 * A source scan retains one cursor and heap node per nonempty position
 * fragment, plus at most one runtime-configured light run and one emission
 * chunk.  Heavy runs are reread from the immutable mapping instead of being
 * copied into scratch.
 */
module;
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <queue>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.indexing.minimizer_fragment_compact_hash32_adapter;

export import bioseq.indexing.compact_hash32_direct_index;
export import bioseq.indexing.minimizer_hashmap;
import bioseq.common.checked_arithmetic;

static_assert(__cplusplus >= 202302L, "C++23-or-later");
static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t),
    "compact-hash32 accounting requires size_t to fit uint64_t");

export namespace bioseq::indexing {

/// Non-owning view assembled at the already-open minimizer-fragment generation boundary.
/// `head` and `manifest` are copied during `open`. The directory view and the
/// owner of `mapped_payload` are borrowed: both must remain alive, immobile,
/// and immutable until every plan/build source replay has returned.
struct opened_minimizer_fragment_generation_view {
    const minimizer_generation_head* head = nullptr;
    const minimizer_generation_manifest* manifest = nullptr;
    const grouped_soa_directory_view* directory = nullptr;
    std::span<const std::byte> mapped_payload{};
};

/// Scratch and emission bounds for compact-hash source replay.
struct minimizer_fragment_compact_hash32_options {
    /// Runs at or below this count are buffered once. Larger runs use three
    /// bounded mapping passes (count, strand zero, strand one). The open path
    /// clamps this to the authenticated minimizer-fragment occurrence cap; callers
    /// normally set it to the compact index's persisted cap.
    std::uint64_t buffered_run_position_cap =
        k_grouped_soa_default_max_occurrence_cap;
    std::size_t emission_chunk_positions = 4'096u;
};

/// Exact retained-memory and replay-cost estimate computed during open.
struct minimizer_fragment_compact_hash32_prebuild_estimate {
    std::uint64_t manifest_fragment_count = 0u;
    std::uint64_t nonempty_fragment_count = 0u;
    std::uint64_t directory_entry_count = 0u;
    std::uint64_t fragment_controls_per_source_scan = 0u;
    std::uint64_t control_scalar_bytes_per_source_scan = 0u;
    /// Persistent O(nonempty fragments) merge plan; no control/posting data.
    std::uint64_t fragment_plan_owner_capacity_bytes = 0u;
    std::uint64_t source_token_owner_bytes = 0u;
    std::uint64_t compact_prebuild_source_scan_count = 2u;
    std::uint64_t prebuild_fragment_controls_read = 0u;
    std::uint64_t prebuild_control_scalar_bytes_read = 0u;
    std::uint64_t minimum_prebuild_position_scalar_reads = 0u;
    std::uint64_t maximum_prebuild_position_scalar_reads = 0u;
    std::uint64_t fragment_cursor_scratch_bytes = 0u;
    std::uint64_t merge_heap_scratch_bytes = 0u;
    std::uint64_t matched_fragment_scratch_bytes = 0u;
    std::uint64_t effective_buffered_run_position_cap = 0u;
    std::uint64_t effective_emission_chunk_position_capacity = 0u;
    std::uint64_t maximum_buffered_run_scratch_bytes = 0u;
    std::uint64_t emission_chunk_scratch_bytes = 0u;
    std::uint64_t maximum_logical_scratch_bytes = 0u;
    /// The adapter borrows the authenticated mapping; this must stay zero.
    std::uint64_t owned_target_control_or_position_bytes = 0u;
};

/// Saturating counters for one or more replayable source scans.
struct minimizer_fragment_compact_hash32_scan_stats {
    std::uint64_t source_scan_attempts = 0u;
    std::uint64_t source_scans_completed = 0u;
    std::uint64_t fragment_controls_read = 0u;
    std::uint64_t control_scalar_bytes_read = 0u;
    std::uint64_t position_scalars_read = 0u;
    std::uint64_t position_scalar_bytes_read = 0u;
    std::uint64_t run_headers_emitted = 0u;
    std::uint64_t positions_emitted = 0u;
    std::uint64_t receiver_position_chunks = 0u;
    std::uint64_t buffered_light_runs = 0u;
    std::uint64_t replayed_heavy_runs = 0u;
    std::uint64_t peak_buffered_run_positions = 0u;
};

/**
 * Adapts authenticated grouped-SoA generation storage into sorted hash runs.
 *
 * The adapter borrows generation storage, retains only fragment cursors, and
 * exposes a replayable source accepted by the compact-hash image builder.
 */
class minimizer_fragment_compact_hash32_adapter {
private:
    struct source_token {
        minimizer_fragment_compact_hash32_adapter* owner = nullptr;
    };

    struct fragment_plan {
        std::size_t entry_begin = 0u;
        std::size_t entry_end = 0u;
        std::uint64_t first_control_byte = 0u;
        std::uint64_t first_position_relative_byte = 0u;
        std::uint64_t core_begin_bp = 0u;
        std::uint64_t core_end_bp = 0u;
    };

    struct decoded_control {
        std::uint32_t hash = 0u;
        std::uint64_t position_begin = 0u;
        std::uint64_t position_end = 0u;
        std::uint64_t exact_count = 0u;
        std::uint64_t position_extent_byte = 0u;
        std::uint64_t core_begin_bp = 0u;
        std::uint64_t core_end_bp = 0u;
        minimizer_scalar_width position_width = minimizer_scalar_width::u16;
    };

    struct fragment_cursor {
        std::size_t entry_ordinal = 0u;
        std::size_t entry_end = 0u;
        std::uint64_t entry_control_byte = 0u;
        std::uint64_t entry_position_relative_byte = 0u;
        std::uint64_t control_ordinal = 0u;
        std::uint64_t core_begin_bp = 0u;
        std::uint64_t core_end_bp = 0u;
        grouped_soa_control_plane_offsets layout{};
        std::uint64_t previous_hash = 0u;
        std::uint64_t previous_position_end = 0u;
        bool have_previous_hash = false;
        bool active = false;
        decoded_control current{};
    };

    struct heap_node {
        std::uint32_t hash = 0u;
        std::uint32_t fragment_ordinal = 0u;
    };

    struct heap_greater {
        [[nodiscard]] bool operator()(
                const heap_node& left,
                const heap_node& right) const noexcept {
            return left.hash != right.hash
                ? left.hash > right.hash
                : left.fragment_ordinal > right.fragment_ordinal;
        }
    };

    minimizer_generation_head head_{};
    minimizer_generation_manifest manifest_{};
    grouped_soa_minimizer_profile profile_{};
    std::span<const grouped_soa_bucket_directory_entry> entries_{};
    std::span<const std::byte> payload_{};
    std::vector<fragment_plan> fragments_{};
    minimizer_fragment_compact_hash32_options options_{};
    minimizer_fragment_compact_hash32_prebuild_estimate estimate_{};
    minimizer_fragment_compact_hash32_scan_stats stats_{};
    std::uint64_t total_control_bytes_ = 0u;
    compact_hash32_geometry geometry_{};
    /// Heap-stable dispatch identity keeps already-issued sources valid when
    /// the adapter is moved (including the move into `std::expected`).
    std::unique_ptr<source_token> source_token_{};
    bool ready_ = false;

    [[nodiscard]] static constexpr bool checked_add_(
            std::uint64_t left,
            std::uint64_t right,
            std::uint64_t& result) noexcept {
        const auto sum = bioseq::common::checked_add(left, right);
        if (!sum.has_value()) {
            return false;
        }
        result = *sum;
        return true;
    }

    [[nodiscard]] static constexpr bool checked_multiply_(
            std::uint64_t left,
            std::uint64_t right,
            std::uint64_t& result) noexcept {
        const auto product = bioseq::common::checked_mul(left, right);
        if (!product.has_value()) {
            return false;
        }
        result = *product;
        return true;
    }

    [[nodiscard]] static constexpr std::uint64_t saturating_add_(
            std::uint64_t left,
            std::uint64_t right) noexcept {
        return bioseq::common::saturating_add(left, right);
    }

    [[nodiscard]] static constexpr std::uint64_t saturating_multiply_(
            std::uint64_t left,
            std::uint64_t right) noexcept {
        return bioseq::common::saturating_mul(left, right);
    }

    static void accumulate_stats_(
            std::uint64_t& destination,
            std::uint64_t value) noexcept {
        destination = saturating_add_(destination, value);
    }

    [[nodiscard]] static constexpr direct_inverted_index_error map_generation_error_(
            minimizer_generation_encoding_error error) noexcept {
        switch (error) {
        case minimizer_generation_encoding_error::arithmetic_overflow:
            return direct_inverted_index_error::arithmetic_overflow;
        case minimizer_generation_encoding_error::control_order:
        case minimizer_generation_encoding_error::directory_order:
        case minimizer_generation_encoding_error::row_order:
            return direct_inverted_index_error::order;
        case minimizer_generation_encoding_error::position_range:
        case minimizer_generation_encoding_error::bp_range:
            return direct_inverted_index_error::posting;
        case minimizer_generation_encoding_error::admission:
            return direct_inverted_index_error::allocation;
        case minimizer_generation_encoding_error::storage_contract_mismatch:
        case minimizer_generation_encoding_error::unsupported_storage_layout:
            return direct_inverted_index_error::storage_contract_mismatch;
        default:
            return direct_inverted_index_error::source_read;
        }
    }

    [[nodiscard]] std::expected<std::uint64_t,
        direct_inverted_index_error>
    read_scalar_(
            std::uint64_t offset,
            minimizer_scalar_width width) const noexcept {
        const std::uint64_t bytes = static_cast<std::uint8_t>(width);
        std::uint64_t end = 0u;
        if (!is_minimizer_scalar_width(width)
            || !checked_add_(offset, bytes, end)
            || end > payload_.size()
            || offset
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::size_t>::max)())) {
            return std::unexpected(direct_inverted_index_error::source_read);
        }
        std::uint64_t value = 0u;
        for (std::uint64_t byte = 0u; byte < bytes; ++byte) {
            value |= static_cast<std::uint64_t>(
                std::to_integer<std::uint8_t>(payload_[
                    static_cast<std::size_t>(offset + byte)]))
                << (byte * 8u);
        }
        return value;
    }

    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    configure_entry_(fragment_cursor& cursor) const noexcept {
        if (cursor.entry_ordinal >= cursor.entry_end
            || cursor.entry_ordinal >= entries_.size()) {
            return std::unexpected(direct_inverted_index_error::source_read);
        }
        const grouped_soa_bucket_directory_entry& entry =
            entries_[cursor.entry_ordinal];
        const auto layout = plan_grouped_soa_control_plane(
            entry.control_element_count,
            profile_.hash_width,
            entry.shape_offset_width,
            profile_.count_width);
        if (!layout || layout->encoded_bytes != entry.control_encoded_bytes) {
            return std::unexpected(layout
                ? direct_inverted_index_error::source_read
                : map_generation_error_(layout.error()));
        }
        cursor.layout = *layout;
        cursor.control_ordinal = 0u;
        return {};
    }

    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    load_current_(
            fragment_cursor& cursor,
            std::uint64_t& controls_seen) noexcept {
        const grouped_soa_bucket_directory_entry& entry =
            entries_[cursor.entry_ordinal];
        const std::uint64_t hash_width = static_cast<std::uint8_t>(
            profile_.hash_width);
        const std::uint64_t offset_width = static_cast<std::uint8_t>(
            entry.shape_offset_width);
        const std::uint64_t count_width = static_cast<std::uint8_t>(
            profile_.count_width);
        std::uint64_t hash_delta = 0u;
        std::uint64_t prefix_delta = 0u;
        std::uint64_t count_delta = 0u;
        std::uint64_t hash_offset = 0u;
        std::uint64_t prefix_offset = 0u;
        std::uint64_t next_prefix_offset = 0u;
        std::uint64_t count_offset = 0u;
        if (cursor.control_ordinal >= entry.control_element_count
            || !checked_multiply_(cursor.control_ordinal,
                hash_width, hash_delta)
            || !checked_multiply_(cursor.control_ordinal,
                offset_width, prefix_delta)
            || !checked_multiply_(cursor.control_ordinal,
                count_width, count_delta)
            || !checked_add_(cursor.entry_control_byte,
                hash_delta, hash_offset)
            || !checked_add_(cursor.entry_control_byte,
                cursor.layout.prefix_offset, prefix_offset)
            || !checked_add_(prefix_offset,
                prefix_delta, prefix_offset)
            || !checked_add_(prefix_offset,
                offset_width, next_prefix_offset)
            || !checked_add_(cursor.entry_control_byte,
                cursor.layout.count_offset, count_offset)
            || !checked_add_(count_offset,
                count_delta, count_offset)) {
            return std::unexpected(
                direct_inverted_index_error::arithmetic_overflow);
        }
        const auto hash = read_scalar_(
            hash_offset, profile_.hash_width);
        const auto begin = read_scalar_(
            prefix_offset, entry.shape_offset_width);
        const auto end = read_scalar_(
            next_prefix_offset, entry.shape_offset_width);
        const auto saturated = read_scalar_(
            count_offset, profile_.count_width);
        if (!hash || !begin || !end || !saturated) {
            return std::unexpected(direct_inverted_index_error::source_read);
        }
        const std::uint64_t cap_plus_one =
            profile_.max_occ_cap + 1u;
        if (*hash > profile_.max_hash
            || *hash > (std::numeric_limits<std::uint32_t>::max)()
            || *end <= *begin
            || *end > entry.position_element_count
            || *saturated != std::min(*end - *begin, cap_plus_one)
            || (cursor.control_ordinal == 0u
                && (*hash != entry.lo_hash || *begin != 0u))
            || (cursor.control_ordinal != 0u
                && *begin != cursor.previous_position_end)
            || (cursor.control_ordinal + 1u
                    == entry.control_element_count
                && (*hash != entry.hi_hash
                    || *end != entry.position_element_count))
            || (cursor.have_previous_hash
                && *hash <= cursor.previous_hash)) {
            return std::unexpected(
                cursor.have_previous_hash && *hash <= cursor.previous_hash
                    ? direct_inverted_index_error::order
                    : direct_inverted_index_error::source_read);
        }
        std::uint64_t position_extent_byte = 0u;
        if (!checked_add_(total_control_bytes_,
                cursor.entry_position_relative_byte,
                position_extent_byte)) {
            return std::unexpected(
                direct_inverted_index_error::arithmetic_overflow);
        }
        cursor.current = decoded_control{
            .hash = static_cast<std::uint32_t>(*hash),
            .position_begin = *begin,
            .position_end = *end,
            .exact_count = *end - *begin,
            .position_extent_byte = position_extent_byte,
            .core_begin_bp = cursor.core_begin_bp,
            .core_end_bp = cursor.core_end_bp,
            .position_width = profile_.position_width,
        };
        cursor.previous_hash = *hash;
        cursor.previous_position_end = *end;
        cursor.have_previous_hash = true;
        cursor.active = true;
        ++controls_seen;
        accumulate_stats_(stats_.fragment_controls_read, 1u);
        accumulate_stats_(stats_.control_scalar_bytes_read,
            hash_width + 2u * offset_width + count_width);
        return {};
    }

    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    initialize_cursor_(
            const fragment_plan& plan,
            fragment_cursor& cursor,
            std::uint64_t& controls_seen) noexcept {
        cursor = fragment_cursor{
            .entry_ordinal = plan.entry_begin,
            .entry_end = plan.entry_end,
            .entry_control_byte = plan.first_control_byte,
            .entry_position_relative_byte =
                plan.first_position_relative_byte,
            .core_begin_bp = plan.core_begin_bp,
            .core_end_bp = plan.core_end_bp,
        };
        if (auto configured = configure_entry_(cursor); !configured) {
            return configured;
        }
        return load_current_(cursor, controls_seen);
    }

    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    advance_cursor_(
            fragment_cursor& cursor,
            std::uint64_t& controls_seen) noexcept {
        const grouped_soa_bucket_directory_entry& entry =
            entries_[cursor.entry_ordinal];
        ++cursor.control_ordinal;
        if (cursor.control_ordinal < entry.control_element_count) {
            return load_current_(cursor, controls_seen);
        }
        if (!checked_add_(cursor.entry_control_byte,
                entry.control_encoded_bytes,
                cursor.entry_control_byte)
            || !checked_add_(cursor.entry_position_relative_byte,
                entry.position_encoded_bytes,
                cursor.entry_position_relative_byte)) {
            return std::unexpected(
                direct_inverted_index_error::arithmetic_overflow);
        }
        ++cursor.entry_ordinal;
        if (cursor.entry_ordinal == cursor.entry_end) {
            cursor.active = false;
            return {};
        }
        if (auto configured = configure_entry_(cursor); !configured) {
            return configured;
        }
        return load_current_(cursor, controls_seen);
    }

    template <class p_visitor>
    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    visit_positions_(
            const decoded_control& control,
            p_visitor&& visitor) {
        const std::uint64_t width = static_cast<std::uint8_t>(
            control.position_width);
        std::optional<std::uint64_t> previous{};
        for (std::uint64_t ordinal = control.position_begin;
             ordinal < control.position_end;
             ++ordinal) {
            std::uint64_t delta = 0u;
            std::uint64_t offset = 0u;
            if (!checked_multiply_(ordinal, width, delta)
                || !checked_add_(control.position_extent_byte,
                    delta, offset)) {
                return std::unexpected(
                    direct_inverted_index_error::arithmetic_overflow);
            }
            const auto local = read_scalar_(offset, control.position_width);
            if (!local) return std::unexpected(local.error());
            const std::uint64_t local_bp = *local >> 1u;
            if (control.core_end_bp <= control.core_begin_bp
                || local_bp
                    >= control.core_end_bp - control.core_begin_bp
                || control.core_begin_bp
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - local_bp) {
                return std::unexpected(direct_inverted_index_error::posting);
            }
            const std::uint64_t global_bp =
                control.core_begin_bp + local_bp;
            if (global_bp
                    > ((std::numeric_limits<std::uint64_t>::max)()
                        >> 1u)) {
                return std::unexpected(
                    direct_inverted_index_error::arithmetic_overflow);
            }
            const std::uint64_t encoded =
                (global_bp << 1u) | (*local & 1u);
            if (previous && encoded <= *previous) {
                return std::unexpected(direct_inverted_index_error::posting);
            }
            previous = encoded;
            accumulate_stats_(stats_.position_scalars_read, 1u);
            accumulate_stats_(stats_.position_scalar_bytes_read, width);
            if (auto accepted = visitor(encoded); !accepted) {
                return accepted;
            }
        }
        return {};
    }

    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    scan_(const compact_hash32_sorted_run_receiver& receiver) {
        ++stats_.source_scan_attempts;
        try {
            std::vector<fragment_cursor> cursors;
            cursors.resize(fragments_.size());
            std::vector<heap_node> heap_storage;
            heap_storage.reserve(fragments_.size());
            std::priority_queue<
                heap_node, std::vector<heap_node>, heap_greater> heap{
                    heap_greater{}, std::move(heap_storage)};
            std::vector<std::uint32_t> matched_fragments;
            matched_fragments.reserve(fragments_.size());
            std::vector<std::uint64_t> run_positions;
            std::vector<std::uint64_t> emission_chunk;
            emission_chunk.reserve(options_.emission_chunk_positions);

            std::uint64_t controls_seen = 0u;
            std::uint64_t logical_positions_emitted = 0u;
            for (std::size_t fragment = 0u;
                 fragment < fragments_.size(); ++fragment) {
                if (auto initialized = initialize_cursor_(
                        fragments_[fragment], cursors[fragment],
                        controls_seen); !initialized) {
                    return initialized;
                }
                heap.push(heap_node{
                    .hash = cursors[fragment].current.hash,
                    .fragment_ordinal =
                        static_cast<std::uint32_t>(fragment),
                });
            }

            while (!heap.empty()) {
                const std::uint32_t hash = heap.top().hash;
                matched_fragments.clear();
                while (!heap.empty() && heap.top().hash == hash) {
                    matched_fragments.push_back(
                        heap.top().fragment_ordinal);
                    heap.pop();
                }
                std::uint64_t global_count = 0u;
                for (const std::uint32_t fragment : matched_fragments) {
                    if (!checked_add_(global_count,
                            cursors[fragment].current.exact_count,
                            global_count)) {
                        return std::unexpected(
                            direct_inverted_index_error::arithmetic_overflow);
                    }
                }
                const bool buffered = global_count
                        <= options_.buffered_run_position_cap
                    && global_count
                        <= static_cast<std::uint64_t>(
                            (std::numeric_limits<std::size_t>::max)());
                run_positions.clear();
                if (buffered) {
                    run_positions.reserve(
                        static_cast<std::size_t>(global_count));
                    ++stats_.buffered_light_runs;
                    stats_.peak_buffered_run_positions = std::max(
                        stats_.peak_buffered_run_positions,
                        global_count);
                } else {
                    ++stats_.replayed_heavy_runs;
                }

                std::uint64_t strand_zero_count = 0u;
                std::uint64_t positions_seen = 0u;
                std::optional<std::uint64_t> previous_global{};
                for (const std::uint32_t fragment : matched_fragments) {
                    const auto counted = visit_positions_(
                        cursors[fragment].current,
                        [&](std::uint64_t encoded)
                                -> std::expected<void,
                                    direct_inverted_index_error> {
                            if (previous_global
                                && encoded < *previous_global) {
                                return std::unexpected(
                                    direct_inverted_index_error::order);
                            }
                            previous_global = encoded;
                            if ((encoded & 1u) == 0u) {
                                ++strand_zero_count;
                            }
                            ++positions_seen;
                            if (buffered) run_positions.push_back(encoded);
                            return {};
                        });
                    if (!counted) return counted;
                }
                if (positions_seen != global_count
                    || run_positions.size()
                        != (buffered
                            ? static_cast<std::size_t>(global_count) : 0u)) {
                    return std::unexpected(
                        direct_inverted_index_error::source_read);
                }

                if (auto begun = receiver.begin_run(
                        compact_hash32_sorted_run_header{
                            .hash = hash,
                            .global_count = global_count,
                            .target_strand_zero_count = strand_zero_count,
                        }); !begun) {
                    return begun;
                }
                ++stats_.run_headers_emitted;
                emission_chunk.clear();
                const auto flush = [&]()
                        -> std::expected<void,
                            direct_inverted_index_error> {
                    if (emission_chunk.empty()) return {};
                    const auto consumed = receiver.consume_positions(
                        emission_chunk);
                    if (!consumed) return consumed;
                    ++stats_.receiver_position_chunks;
                    emission_chunk.clear();
                    return {};
                };
                const auto emit = [&](std::uint64_t encoded)
                        -> std::expected<void,
                            direct_inverted_index_error> {
                    emission_chunk.push_back(encoded);
                    ++logical_positions_emitted;
                    ++stats_.positions_emitted;
                    if (emission_chunk.size()
                            == options_.emission_chunk_positions) {
                        return flush();
                    }
                    return {};
                };

                for (std::uint32_t strand = 0u; strand < 2u; ++strand) {
                    const bool strand_one = strand != 0u;
                    std::optional<std::uint64_t> previous_strand{};
                    if (buffered) {
                        for (const std::uint64_t encoded : run_positions) {
                            if (((encoded & 1u) != 0u) != strand_one) {
                                continue;
                            }
                            if (previous_strand
                                && encoded < *previous_strand) {
                                return std::unexpected(
                                    direct_inverted_index_error::order);
                            }
                            previous_strand = encoded;
                            if (auto emitted = emit(encoded); !emitted) {
                                return emitted;
                            }
                        }
                    } else {
                        for (const std::uint32_t fragment
                                : matched_fragments) {
                            const auto emitted_positions = visit_positions_(
                                cursors[fragment].current,
                                [&](std::uint64_t encoded)
                                        -> std::expected<void,
                                            direct_inverted_index_error> {
                                    if (((encoded & 1u) != 0u)
                                            != strand_one) {
                                        return {};
                                    }
                                    if (previous_strand
                                        && encoded < *previous_strand) {
                                        return std::unexpected(
                                            direct_inverted_index_error::order);
                                    }
                                    previous_strand = encoded;
                                    return emit(encoded);
                                });
                            if (!emitted_positions) {
                                return emitted_positions;
                            }
                        }
                    }
                    if (auto flushed = flush(); !flushed) return flushed;
                }

                for (const std::uint32_t fragment : matched_fragments) {
                    if (auto advanced = advance_cursor_(
                            cursors[fragment], controls_seen); !advanced) {
                        return advanced;
                    }
                    if (cursors[fragment].active) {
                        heap.push(heap_node{
                            .hash = cursors[fragment].current.hash,
                            .fragment_ordinal = fragment,
                        });
                    }
                }
            }
            if (controls_seen != manifest_.control_count
                || logical_positions_emitted != manifest_.position_count) {
                return std::unexpected(
                    direct_inverted_index_error::source_read);
            }
            ++stats_.source_scans_completed;
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(direct_inverted_index_error::allocation);
        } catch (const std::length_error&) {
            return std::unexpected(direct_inverted_index_error::allocation);
        }
    }

    [[nodiscard]] static std::expected<void, direct_inverted_index_error>
    scan_thunk_(
            void* context,
            const compact_hash32_sorted_run_receiver& receiver) {
        if (context == nullptr) {
            return std::unexpected(direct_inverted_index_error::source_read);
        }
        auto& token = *static_cast<source_token*>(context);
        if (token.owner == nullptr || !token.owner->ready_) {
            return std::unexpected(direct_inverted_index_error::source_read);
        }
        return token.owner->scan_(receiver);
    }

public:
    minimizer_fragment_compact_hash32_adapter() = default;
    minimizer_fragment_compact_hash32_adapter(
            minimizer_fragment_compact_hash32_adapter&& other) noexcept
        : head_(std::move(other.head_)),
          manifest_(std::move(other.manifest_)),
          profile_(std::move(other.profile_)),
          entries_(other.entries_),
          payload_(other.payload_),
          fragments_(std::move(other.fragments_)),
          options_(other.options_),
          estimate_(other.estimate_),
          stats_(other.stats_),
          total_control_bytes_(other.total_control_bytes_),
          geometry_(other.geometry_),
          source_token_(std::move(other.source_token_)),
          ready_(other.ready_) {
        if (source_token_ != nullptr) source_token_->owner = this;
        other.entries_ = {};
        other.payload_ = {};
        other.total_control_bytes_ = 0u;
        other.ready_ = false;
    }
    minimizer_fragment_compact_hash32_adapter& operator=(
        minimizer_fragment_compact_hash32_adapter&&) = delete;
    minimizer_fragment_compact_hash32_adapter(const minimizer_fragment_compact_hash32_adapter&) = delete;
    minimizer_fragment_compact_hash32_adapter& operator=(
        const minimizer_fragment_compact_hash32_adapter&) = delete;

    [[nodiscard]] static std::expected<
        minimizer_fragment_compact_hash32_adapter,
        minimizer_generation_encoding_error>
    open(
            const opened_minimizer_fragment_generation_view& generation,
            minimizer_fragment_compact_hash32_options options = {}) {
        if (generation.head == nullptr
            || generation.manifest == nullptr
            || generation.directory == nullptr
            || options.emission_chunk_positions == 0u) {
            return std::unexpected(
                minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        const minimizer_generation_head& head = *generation.head;
        const minimizer_generation_manifest& manifest = *generation.manifest;
        const auto profile = derive_grouped_soa_minimizer_profile(manifest);
        if (!profile
            || manifest.layout
                != minimizer_generation_layout::grouped_soa_segment_local
            || head.layout != manifest.layout
            || head.generation_id != manifest.generation_id
            || head.source_identity != manifest.source_identity
            || head.source_content_checksum
                != manifest.source_content_checksum
            || head.generation_id == 0u
            || head.source_identity == 0u
            || head.storage_contract_checksum == 0u
            || generation.directory->profile() != *profile
            || generation.mapped_payload.size() != manifest.payload_bytes
            || profile->max_hash
                > (std::numeric_limits<std::uint32_t>::max)()
            || profile->max_occ_cap == 0u
            || profile->max_occ_cap
                > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected(
                minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        const std::uint32_t max_hash =
            static_cast<std::uint32_t>(profile->max_hash);
        const std::uint8_t hash_bits = static_cast<std::uint8_t>(
            std::bit_width(max_hash));
        const std::uint64_t expected_max_hash = hash_bits == 32u
            ? (std::numeric_limits<std::uint32_t>::max)()
            : hash_bits == 0u ? 0u
            : (std::uint64_t{1u} << hash_bits) - 1u;
        if (hash_bits < 9u || profile->max_hash != expected_max_hash) {
            return std::unexpected(
                minimizer_generation_encoding_error::storage_contract_mismatch);
        }

        minimizer_fragment_compact_hash32_adapter adapter;
        adapter.head_ = head;
        adapter.manifest_ = manifest;
        adapter.profile_ = *profile;
        adapter.entries_ = generation.directory->entries();
        adapter.payload_ = generation.mapped_payload;
        adapter.options_ = options;
        const std::uint64_t size_t_position_cap =
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)());
        adapter.options_.buffered_run_position_cap = std::min(
            std::min(adapter.options_.buffered_run_position_cap,
                profile->max_occ_cap),
            std::min(manifest.position_count, size_t_position_cap));
        const std::uint64_t bounded_emission_chunk_positions = std::min(
            static_cast<std::uint64_t>(
                adapter.options_.emission_chunk_positions),
            std::min(manifest.position_count, size_t_position_cap));
        // A zero-position generation emits nothing, but retaining a one-slot
        // threshold keeps the scan invariant nonzero and makes reserve,
        // threshold, and accounting use one identical effective capacity.
        adapter.options_.emission_chunk_positions =
            static_cast<std::size_t>(
                bounded_emission_chunk_positions == 0u
                    ? 1u : bounded_emission_chunk_positions);
        adapter.geometry_ = compact_hash32_geometry{
            .hash_domain_bits = hash_bits,
            .max_hash = max_hash,
        };
        if (adapter.entries_.size()
                > (std::numeric_limits<std::uint32_t>::max)() / 2u) {
            return std::unexpected(
                minimizer_generation_encoding_error::directory_size);
        }
        std::size_t nonempty_fragment_count = 0u;
        std::int32_t counted_previous_segment = 0;
        bool counted_any_segment = false;
        for (const grouped_soa_bucket_directory_entry& entry
                : adapter.entries_) {
            if (entry.position_segment < 0
                || static_cast<std::uint32_t>(entry.position_segment)
                    >= manifest.position_segment_count) {
                return std::unexpected(
                    minimizer_generation_encoding_error::position_segment);
            }
            if (!counted_any_segment
                || entry.position_segment != counted_previous_segment) {
                ++nonempty_fragment_count;
                counted_previous_segment = entry.position_segment;
                counted_any_segment = true;
            }
        }
        try {
            adapter.fragments_.reserve(nonempty_fragment_count);
        } catch (const std::bad_alloc&) {
            return std::unexpected(
                minimizer_generation_encoding_error::admission);
        } catch (const std::length_error&) {
            return std::unexpected(
                minimizer_generation_encoding_error::admission);
        }

        std::uint64_t control_bytes = 0u;
        std::uint64_t position_bytes = 0u;
        std::uint64_t control_count = 0u;
        std::uint64_t position_count = 0u;
        std::uint64_t control_scalar_bytes = 0u;
        std::int32_t previous_segment = -1;
        for (std::size_t ordinal = 0u;
             ordinal < adapter.entries_.size(); ++ordinal) {
            const grouped_soa_bucket_directory_entry& entry =
                adapter.entries_[ordinal];
            if (entry.position_segment < 0
                || static_cast<std::uint32_t>(entry.position_segment)
                    >= manifest.position_segment_count) {
                return std::unexpected(
                    minimizer_generation_encoding_error::position_segment);
            }
            const auto layout = plan_grouped_soa_control_plane(
                entry.control_element_count,
                profile->hash_width,
                entry.shape_offset_width,
                profile->count_width);
            std::uint64_t expected_position_bytes = 0u;
            std::uint64_t scalar_bytes_per_control = 0u;
            std::uint64_t entry_scalar_bytes = 0u;
            if (!layout) return std::unexpected(layout.error());
            if (layout->encoded_bytes != entry.control_encoded_bytes) {
                return std::unexpected(
                    minimizer_generation_encoding_error::byte_count);
            }
            if (!checked_multiply_(entry.position_element_count,
                    static_cast<std::uint8_t>(
                        profile->position_width),
                    expected_position_bytes)
                || !checked_add_(
                    static_cast<std::uint8_t>(
                        profile->hash_width),
                    static_cast<std::uint8_t>(
                        profile->count_width),
                    scalar_bytes_per_control)
                || !checked_add_(scalar_bytes_per_control,
                    2u * static_cast<std::uint8_t>(
                        entry.shape_offset_width),
                    scalar_bytes_per_control)
                || !checked_multiply_(entry.control_element_count,
                    scalar_bytes_per_control, entry_scalar_bytes)) {
                return std::unexpected(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
            if (expected_position_bytes
                    != entry.position_encoded_bytes) {
                return std::unexpected(
                    minimizer_generation_encoding_error::byte_count);
            }
            if (entry.position_segment != previous_segment) {
                const auto core = derive_minimizer_position_segment_bounds(
                    manifest,
                    static_cast<std::uint32_t>(entry.position_segment));
                if (!core) return std::unexpected(core.error());
                try {
                    adapter.fragments_.push_back(fragment_plan{
                        .entry_begin = ordinal,
                        .entry_end = ordinal + 1u,
                        .first_control_byte = control_bytes,
                        .first_position_relative_byte = position_bytes,
                        .core_begin_bp = core->begin_bp,
                        .core_end_bp = core->end_bp,
                    });
                } catch (const std::bad_alloc&) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::admission);
                } catch (const std::length_error&) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::admission);
                }
                previous_segment = entry.position_segment;
            } else {
                adapter.fragments_.back().entry_end = ordinal + 1u;
            }
            if (!checked_add_(control_bytes,
                    entry.control_encoded_bytes, control_bytes)
                || !checked_add_(position_bytes,
                    entry.position_encoded_bytes, position_bytes)
                || !checked_add_(control_count,
                    entry.control_element_count, control_count)
                || !checked_add_(position_count,
                    entry.position_element_count, position_count)
                || !checked_add_(control_scalar_bytes,
                    entry_scalar_bytes, control_scalar_bytes)) {
                return std::unexpected(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
        }
        std::uint64_t payload_bytes = 0u;
        if (!checked_add_(control_bytes, position_bytes, payload_bytes)
            || payload_bytes != manifest.payload_bytes
            || control_count != manifest.control_count
            || position_count != manifest.position_count
            || manifest.extent_count
                != static_cast<std::uint32_t>(
                    adapter.entries_.size() * 2u)) {
            return std::unexpected(
                minimizer_generation_encoding_error::element_count);
        }
        adapter.total_control_bytes_ = control_bytes;

        minimizer_fragment_compact_hash32_prebuild_estimate estimate{
            .manifest_fragment_count = manifest.position_segment_count,
            .nonempty_fragment_count = adapter.fragments_.size(),
            .directory_entry_count = adapter.entries_.size(),
            .fragment_controls_per_source_scan = manifest.control_count,
            .control_scalar_bytes_per_source_scan = control_scalar_bytes,
            .fragment_plan_owner_capacity_bytes = saturating_multiply_(
                adapter.fragments_.capacity(), sizeof(fragment_plan)),
            .source_token_owner_bytes = sizeof(source_token),
            .compact_prebuild_source_scan_count = 2u,
            .prebuild_fragment_controls_read = saturating_multiply_(
                manifest.control_count, 2u),
            .prebuild_control_scalar_bytes_read = saturating_multiply_(
                control_scalar_bytes, 2u),
            .minimum_prebuild_position_scalar_reads = saturating_multiply_(
                manifest.position_count, 2u),
            .maximum_prebuild_position_scalar_reads = saturating_multiply_(
                manifest.position_count, 6u),
            .fragment_cursor_scratch_bytes = saturating_multiply_(
                adapter.fragments_.size(), sizeof(fragment_cursor)),
            .merge_heap_scratch_bytes = saturating_multiply_(
                adapter.fragments_.size(), sizeof(heap_node)),
            .matched_fragment_scratch_bytes = saturating_multiply_(
                adapter.fragments_.size(), sizeof(std::uint32_t)),
            .effective_buffered_run_position_cap =
                adapter.options_.buffered_run_position_cap,
            .effective_emission_chunk_position_capacity =
                adapter.options_.emission_chunk_positions,
            .maximum_buffered_run_scratch_bytes = saturating_multiply_(
                adapter.options_.buffered_run_position_cap,
                sizeof(std::uint64_t)),
            .emission_chunk_scratch_bytes = saturating_multiply_(
                adapter.options_.emission_chunk_positions,
                sizeof(std::uint64_t)),
            .owned_target_control_or_position_bytes = 0u,
        };
        estimate.maximum_logical_scratch_bytes = saturating_add_(
            saturating_add_(
                saturating_add_(estimate.fragment_cursor_scratch_bytes,
                    estimate.merge_heap_scratch_bytes),
                estimate.matched_fragment_scratch_bytes),
            saturating_add_(estimate.maximum_buffered_run_scratch_bytes,
                estimate.emission_chunk_scratch_bytes));
        adapter.estimate_ = estimate;
        try {
            adapter.source_token_ = std::make_unique<source_token>();
        } catch (const std::bad_alloc&) {
            return std::unexpected(
                minimizer_generation_encoding_error::admission);
        }
        adapter.source_token_->owner = &adapter;
        adapter.ready_ = true;
        return std::move(adapter);
    }

    /// The returned source borrows this adapter, its grouped-SoA directory,
    /// and its mapped payload owner through both compact-index replays. Adapter moves are safe
    /// because dispatch uses a heap-stable token; destruction is invalidation.
    [[nodiscard]] compact_hash32_sorted_run_source sorted_run_source() noexcept {
        return compact_hash32_sorted_run_source{
            .context = ready_ && source_token_ != nullptr
                ? static_cast<void*>(source_token_.get()) : nullptr,
            .scan_sorted_runs_fn = &scan_thunk_,
        };
    }

    [[nodiscard]] std::expected<compact_hash32_build_options,
        direct_inverted_index_error>
    compact_build_options(
            std::uint32_t persisted_max_occ_cap,
        std::uint64_t max_image_bytes =
                (std::numeric_limits<std::uint64_t>::max)()) const noexcept {
        if (!ready_ || persisted_max_occ_cap == 0u
            || persisted_max_occ_cap > profile_.max_occ_cap) {
            return std::unexpected(
                direct_inverted_index_error::storage_contract_mismatch);
        }
        return compact_hash32_build_options{
            .identity = direct_index_identity{
                .generation_id = head_.generation_id,
                .source_identity = head_.source_identity,
                .storage_contract_checksum = head_.storage_contract_checksum,
                .target_length = manifest_.target_length,
            },
            .max_occ_cap = persisted_max_occ_cap,
            .geometry = geometry_,
            .max_image_bytes = max_image_bytes,
        };
    }

    [[nodiscard]] bool ready() const noexcept { return ready_; }

    [[nodiscard]] const minimizer_fragment_compact_hash32_prebuild_estimate&
    prebuild_estimate() const noexcept { return estimate_; }

    [[nodiscard]] const minimizer_fragment_compact_hash32_scan_stats&
    scan_stats() const noexcept { return stats_; }

    [[nodiscard]] compact_hash32_geometry geometry() const noexcept {
        return geometry_;
    }
};

} // namespace bioseq::indexing
