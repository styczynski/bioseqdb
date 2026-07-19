/**
 * @file minimizer_format5_compact_h32_adapter.cppm
 * @brief Replayable Format-5/Shape-V5 to compact exact-H32 run adapter.
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

export module bioseq.indexing.minimizer_format5_compact_h32_adapter;

export import bioseq.indexing.minimizer_direct_inverted_index;
export import bioseq.indexing.minimizer_hashmap;

static_assert(__cplusplus >= 202302L, "C++23-or-later");
static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t),
    "compact-H32 accounting requires size_t to fit uint64_t");

export namespace bioseq::indexing {

/// Non-owning view assembled at the already-open Format-5 generation boundary.
/// `head` and `manifest` are copied during `open`. The `DirView` object and the
/// owner of `mapped_payload` are borrowed: both must remain alive, immobile,
/// and immutable until every plan/build source replay has returned.
struct Format5CompactH32OpenedGenerationView {
    const MinimizerGenerationHead* head = nullptr;
    const MinimizerGenerationManifest* manifest = nullptr;
    const DirView* directory = nullptr;
    std::span<const std::byte> mapped_payload{};
};

struct Format5CompactH32AdapterOptions {
    /// Runs at or below this count are buffered once. Larger runs use three
    /// bounded mapping passes (count, strand zero, strand one). The open path
    /// clamps this to the authenticated Format-5 occurrence cap; callers
    /// normally set it to the compact index's persisted cap.
    std::uint64_t buffered_run_position_cap =
        kMinimizerShapeV5DefaultMaxOccCap;
    std::size_t emission_chunk_positions = 4'096u;
};

struct Format5CompactH32PrebuildEstimate {
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

struct Format5CompactH32ScanEvidence {
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

class Format5CompactH32Adapter {
private:
    struct SourceToken {
        Format5CompactH32Adapter* owner = nullptr;
    };

    struct FragmentPlan {
        std::size_t entry_begin = 0u;
        std::size_t entry_end = 0u;
        std::uint64_t first_control_byte = 0u;
        std::uint64_t first_position_relative_byte = 0u;
        std::uint64_t core_begin_bp = 0u;
        std::uint64_t core_end_bp = 0u;
    };

    struct DecodedControl {
        std::uint32_t hash = 0u;
        std::uint64_t position_begin = 0u;
        std::uint64_t position_end = 0u;
        std::uint64_t exact_count = 0u;
        std::uint64_t position_extent_byte = 0u;
        std::uint64_t core_begin_bp = 0u;
        std::uint64_t core_end_bp = 0u;
        MinimizerScalarWidth position_width = MinimizerScalarWidth::u16;
    };

    struct FragmentCursor {
        std::size_t entry_ordinal = 0u;
        std::size_t entry_end = 0u;
        std::uint64_t entry_control_byte = 0u;
        std::uint64_t entry_position_relative_byte = 0u;
        std::uint64_t control_ordinal = 0u;
        std::uint64_t core_begin_bp = 0u;
        std::uint64_t core_end_bp = 0u;
        MinimizerShapeV5ControlLayoutOffsets layout{};
        std::uint64_t previous_hash = 0u;
        std::uint64_t previous_position_end = 0u;
        bool have_previous_hash = false;
        bool active = false;
        DecodedControl current{};
    };

    struct HeapNode {
        std::uint32_t hash = 0u;
        std::uint32_t fragment_ordinal = 0u;
    };

    struct HeapGreater {
        [[nodiscard]] bool operator()(
                const HeapNode& left,
                const HeapNode& right) const noexcept {
            return left.hash != right.hash
                ? left.hash > right.hash
                : left.fragment_ordinal > right.fragment_ordinal;
        }
    };

    MinimizerGenerationHead head_{};
    MinimizerGenerationManifest manifest_{};
    MinimizerGenerationPayloadProfile profile_{};
    std::span<const MinimizerBucketDirEntry> entries_{};
    std::span<const std::byte> payload_{};
    std::vector<FragmentPlan> fragments_{};
    Format5CompactH32AdapterOptions options_{};
    Format5CompactH32PrebuildEstimate estimate_{};
    Format5CompactH32ScanEvidence evidence_{};
    std::uint64_t total_control_bytes_ = 0u;
    CompactH32Geometry geometry_{};
    /// Heap-stable dispatch identity keeps already-issued sources valid when
    /// the adapter is moved (including the move into `std::expected`).
    std::unique_ptr<SourceToken> source_token_{};
    bool ready_ = false;

    [[nodiscard]] static constexpr bool checked_add_(
            std::uint64_t left,
            std::uint64_t right,
            std::uint64_t& result) noexcept {
        if (left > (std::numeric_limits<std::uint64_t>::max)() - right) {
            return false;
        }
        result = left + right;
        return true;
    }

    [[nodiscard]] static constexpr bool checked_multiply_(
            std::uint64_t left,
            std::uint64_t right,
            std::uint64_t& result) noexcept {
        if (right != 0u
            && left > (std::numeric_limits<std::uint64_t>::max)() / right) {
            return false;
        }
        result = left * right;
        return true;
    }

    [[nodiscard]] static constexpr std::uint64_t saturating_add_(
            std::uint64_t left,
            std::uint64_t right) noexcept {
        std::uint64_t result = 0u;
        return checked_add_(left, right, result)
            ? result : (std::numeric_limits<std::uint64_t>::max)();
    }

    [[nodiscard]] static constexpr std::uint64_t saturating_multiply_(
            std::uint64_t left,
            std::uint64_t right) noexcept {
        std::uint64_t result = 0u;
        return checked_multiply_(left, right, result)
            ? result : (std::numeric_limits<std::uint64_t>::max)();
    }

    static void add_evidence_(
            std::uint64_t& destination,
            std::uint64_t value) noexcept {
        destination = saturating_add_(destination, value);
    }

    [[nodiscard]] static constexpr DirectInvertedIndexError map_wire_error_(
            MinimizerGenerationWireError error) noexcept {
        switch (error) {
        case MinimizerGenerationWireError::arithmetic_overflow:
            return DirectInvertedIndexError::arithmetic_overflow;
        case MinimizerGenerationWireError::control_order:
        case MinimizerGenerationWireError::directory_order:
        case MinimizerGenerationWireError::row_order:
            return DirectInvertedIndexError::order;
        case MinimizerGenerationWireError::position_range:
        case MinimizerGenerationWireError::bp_range:
            return DirectInvertedIndexError::posting;
        case MinimizerGenerationWireError::admission:
            return DirectInvertedIndexError::allocation;
        case MinimizerGenerationWireError::compatibility_mismatch:
        case MinimizerGenerationWireError::version:
            return DirectInvertedIndexError::compatibility;
        default:
            return DirectInvertedIndexError::source_read;
        }
    }

    [[nodiscard]] std::expected<std::uint64_t,
        DirectInvertedIndexError>
    read_scalar_(
            std::uint64_t offset,
            MinimizerScalarWidth width) const noexcept {
        const std::uint64_t bytes = static_cast<std::uint8_t>(width);
        std::uint64_t end = 0u;
        if (!is_minimizer_scalar_width(width)
            || !checked_add_(offset, bytes, end)
            || end > payload_.size()
            || offset
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::size_t>::max)())) {
            return std::unexpected(DirectInvertedIndexError::source_read);
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

    [[nodiscard]] std::expected<void, DirectInvertedIndexError>
    configure_entry_(FragmentCursor& cursor) const noexcept {
        if (cursor.entry_ordinal >= cursor.entry_end
            || cursor.entry_ordinal >= entries_.size()) {
            return std::unexpected(DirectInvertedIndexError::source_read);
        }
        const MinimizerBucketDirEntry& entry =
            entries_[cursor.entry_ordinal];
        const auto layout = minimizer_shape_v5_control_layout(
            entry.control_element_count,
            profile_.shape_v5->hash_width,
            entry.shape_offset_width,
            profile_.shape_v5->count_width);
        if (!layout || layout->encoded_bytes != entry.control_encoded_bytes) {
            return std::unexpected(layout
                ? DirectInvertedIndexError::source_read
                : map_wire_error_(layout.error()));
        }
        cursor.layout = *layout;
        cursor.control_ordinal = 0u;
        return {};
    }

    [[nodiscard]] std::expected<void, DirectInvertedIndexError>
    load_current_(
            FragmentCursor& cursor,
            std::uint64_t& controls_seen) noexcept {
        const MinimizerBucketDirEntry& entry =
            entries_[cursor.entry_ordinal];
        const std::uint64_t hash_width = static_cast<std::uint8_t>(
            profile_.shape_v5->hash_width);
        const std::uint64_t offset_width = static_cast<std::uint8_t>(
            entry.shape_offset_width);
        const std::uint64_t count_width = static_cast<std::uint8_t>(
            profile_.shape_v5->count_width);
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
                DirectInvertedIndexError::arithmetic_overflow);
        }
        const auto hash = read_scalar_(
            hash_offset, profile_.shape_v5->hash_width);
        const auto begin = read_scalar_(
            prefix_offset, entry.shape_offset_width);
        const auto end = read_scalar_(
            next_prefix_offset, entry.shape_offset_width);
        const auto saturated = read_scalar_(
            count_offset, profile_.shape_v5->count_width);
        if (!hash || !begin || !end || !saturated) {
            return std::unexpected(DirectInvertedIndexError::source_read);
        }
        const std::uint64_t cap_plus_one =
            profile_.shape_v5->max_occ_cap + 1u;
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
                    ? DirectInvertedIndexError::order
                    : DirectInvertedIndexError::source_read);
        }
        std::uint64_t position_extent_byte = 0u;
        if (!checked_add_(total_control_bytes_,
                cursor.entry_position_relative_byte,
                position_extent_byte)) {
            return std::unexpected(
                DirectInvertedIndexError::arithmetic_overflow);
        }
        cursor.current = DecodedControl{
            .hash = static_cast<std::uint32_t>(*hash),
            .position_begin = *begin,
            .position_end = *end,
            .exact_count = *end - *begin,
            .position_extent_byte = position_extent_byte,
            .core_begin_bp = cursor.core_begin_bp,
            .core_end_bp = cursor.core_end_bp,
            .position_width = profile_.shape_v5->position_width,
        };
        cursor.previous_hash = *hash;
        cursor.previous_position_end = *end;
        cursor.have_previous_hash = true;
        cursor.active = true;
        ++controls_seen;
        add_evidence_(evidence_.fragment_controls_read, 1u);
        add_evidence_(evidence_.control_scalar_bytes_read,
            hash_width + 2u * offset_width + count_width);
        return {};
    }

    [[nodiscard]] std::expected<void, DirectInvertedIndexError>
    initialize_cursor_(
            const FragmentPlan& plan,
            FragmentCursor& cursor,
            std::uint64_t& controls_seen) noexcept {
        cursor = FragmentCursor{
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

    [[nodiscard]] std::expected<void, DirectInvertedIndexError>
    advance_cursor_(
            FragmentCursor& cursor,
            std::uint64_t& controls_seen) noexcept {
        const MinimizerBucketDirEntry& entry =
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
                DirectInvertedIndexError::arithmetic_overflow);
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

    template <class Visitor>
    [[nodiscard]] std::expected<void, DirectInvertedIndexError>
    visit_positions_(
            const DecodedControl& control,
            Visitor&& visitor) {
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
                    DirectInvertedIndexError::arithmetic_overflow);
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
                return std::unexpected(DirectInvertedIndexError::posting);
            }
            const std::uint64_t global_bp =
                control.core_begin_bp + local_bp;
            if (global_bp
                    > ((std::numeric_limits<std::uint64_t>::max)()
                        >> 1u)) {
                return std::unexpected(
                    DirectInvertedIndexError::arithmetic_overflow);
            }
            const std::uint64_t encoded =
                (global_bp << 1u) | (*local & 1u);
            if (previous && encoded <= *previous) {
                return std::unexpected(DirectInvertedIndexError::posting);
            }
            previous = encoded;
            add_evidence_(evidence_.position_scalars_read, 1u);
            add_evidence_(evidence_.position_scalar_bytes_read, width);
            if (auto accepted = visitor(encoded); !accepted) {
                return accepted;
            }
        }
        return {};
    }

    [[nodiscard]] std::expected<void, DirectInvertedIndexError>
    scan_(const CompactH32SortedRunReceiver& receiver) {
        ++evidence_.source_scan_attempts;
        try {
            std::vector<FragmentCursor> cursors;
            cursors.resize(fragments_.size());
            std::vector<HeapNode> heap_storage;
            heap_storage.reserve(fragments_.size());
            std::priority_queue<
                HeapNode, std::vector<HeapNode>, HeapGreater> heap{
                    HeapGreater{}, std::move(heap_storage)};
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
                heap.push(HeapNode{
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
                            DirectInvertedIndexError::arithmetic_overflow);
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
                    ++evidence_.buffered_light_runs;
                    evidence_.peak_buffered_run_positions = std::max(
                        evidence_.peak_buffered_run_positions,
                        global_count);
                } else {
                    ++evidence_.replayed_heavy_runs;
                }

                std::uint64_t strand_zero_count = 0u;
                std::uint64_t positions_seen = 0u;
                std::optional<std::uint64_t> previous_global{};
                for (const std::uint32_t fragment : matched_fragments) {
                    const auto counted = visit_positions_(
                        cursors[fragment].current,
                        [&](std::uint64_t encoded)
                                -> std::expected<void,
                                    DirectInvertedIndexError> {
                            if (previous_global
                                && encoded < *previous_global) {
                                return std::unexpected(
                                    DirectInvertedIndexError::order);
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
                        DirectInvertedIndexError::source_read);
                }

                if (auto begun = receiver.begin_run(
                        CompactH32SortedRunHeader{
                            .hash = hash,
                            .global_count = global_count,
                            .target_strand_zero_count = strand_zero_count,
                        }); !begun) {
                    return begun;
                }
                ++evidence_.run_headers_emitted;
                emission_chunk.clear();
                const auto flush = [&]()
                        -> std::expected<void,
                            DirectInvertedIndexError> {
                    if (emission_chunk.empty()) return {};
                    const auto consumed = receiver.consume_positions(
                        emission_chunk);
                    if (!consumed) return consumed;
                    ++evidence_.receiver_position_chunks;
                    emission_chunk.clear();
                    return {};
                };
                const auto emit = [&](std::uint64_t encoded)
                        -> std::expected<void,
                            DirectInvertedIndexError> {
                    emission_chunk.push_back(encoded);
                    ++logical_positions_emitted;
                    ++evidence_.positions_emitted;
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
                                    DirectInvertedIndexError::order);
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
                                            DirectInvertedIndexError> {
                                    if (((encoded & 1u) != 0u)
                                            != strand_one) {
                                        return {};
                                    }
                                    if (previous_strand
                                        && encoded < *previous_strand) {
                                        return std::unexpected(
                                            DirectInvertedIndexError::order);
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
                        heap.push(HeapNode{
                            .hash = cursors[fragment].current.hash,
                            .fragment_ordinal = fragment,
                        });
                    }
                }
            }
            if (controls_seen != manifest_.control_count
                || logical_positions_emitted != manifest_.position_count) {
                return std::unexpected(
                    DirectInvertedIndexError::source_read);
            }
            ++evidence_.source_scans_completed;
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(DirectInvertedIndexError::allocation);
        } catch (const std::length_error&) {
            return std::unexpected(DirectInvertedIndexError::allocation);
        }
    }

    [[nodiscard]] static std::expected<void, DirectInvertedIndexError>
    scan_thunk_(
            void* context,
            const CompactH32SortedRunReceiver& receiver) {
        if (context == nullptr) {
            return std::unexpected(DirectInvertedIndexError::source_read);
        }
        auto& token = *static_cast<SourceToken*>(context);
        if (token.owner == nullptr || !token.owner->ready_) {
            return std::unexpected(DirectInvertedIndexError::source_read);
        }
        return token.owner->scan_(receiver);
    }

public:
    Format5CompactH32Adapter() = default;
    Format5CompactH32Adapter(
            Format5CompactH32Adapter&& other) noexcept
        : head_(std::move(other.head_)),
          manifest_(std::move(other.manifest_)),
          profile_(std::move(other.profile_)),
          entries_(other.entries_),
          payload_(other.payload_),
          fragments_(std::move(other.fragments_)),
          options_(other.options_),
          estimate_(other.estimate_),
          evidence_(other.evidence_),
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
    Format5CompactH32Adapter& operator=(
        Format5CompactH32Adapter&&) = delete;
    Format5CompactH32Adapter(const Format5CompactH32Adapter&) = delete;
    Format5CompactH32Adapter& operator=(
        const Format5CompactH32Adapter&) = delete;

    [[nodiscard]] static std::expected<
        Format5CompactH32Adapter,
        MinimizerGenerationWireError>
    open(
            const Format5CompactH32OpenedGenerationView& generation,
            Format5CompactH32AdapterOptions options = {}) {
        if (generation.head == nullptr
            || generation.manifest == nullptr
            || generation.directory == nullptr
            || options.emission_chunk_positions == 0u) {
            return std::unexpected(
                MinimizerGenerationWireError::compatibility_mismatch);
        }
        const MinimizerGenerationHead& head = *generation.head;
        const MinimizerGenerationManifest& manifest = *generation.manifest;
        const auto profile = minimizer_generation_payload_profile(manifest);
        if (!profile || !profile->shape_v5
            || manifest.wire_format
                != MinimizerGenerationWireFormat::
                    grouped_shape_soa_segment_local
            || head.wire_format != manifest.wire_format
            || head.generation_id != manifest.generation_id
            || head.source_identity != manifest.source_identity
            || head.source_content_checksum
                != manifest.source_content_checksum
            || head.generation_id == 0u
            || head.source_identity == 0u
            || head.compatibility_checksum == 0u
            || generation.directory->profile() != *profile
            || generation.mapped_payload.size() != manifest.payload_bytes
            || profile->max_hash
                > (std::numeric_limits<std::uint32_t>::max)()
            || profile->shape_v5->max_occ_cap == 0u
            || profile->shape_v5->max_occ_cap
                > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected(
                MinimizerGenerationWireError::compatibility_mismatch);
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
                MinimizerGenerationWireError::compatibility_mismatch);
        }

        Format5CompactH32Adapter adapter;
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
                profile->shape_v5->max_occ_cap),
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
        adapter.geometry_ = CompactH32Geometry{
            .hash_domain_bits = hash_bits,
            .max_hash = max_hash,
        };
        if (adapter.entries_.size()
                > (std::numeric_limits<std::uint32_t>::max)() / 2u) {
            return std::unexpected(
                MinimizerGenerationWireError::directory_size);
        }
        std::size_t nonempty_fragment_count = 0u;
        std::int32_t counted_previous_segment = 0;
        bool counted_any_segment = false;
        for (const MinimizerBucketDirEntry& entry : adapter.entries_) {
            if (entry.position_segment < 0
                || static_cast<std::uint32_t>(entry.position_segment)
                    >= manifest.position_segment_count) {
                return std::unexpected(
                    MinimizerGenerationWireError::position_segment);
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
            return std::unexpected(MinimizerGenerationWireError::admission);
        } catch (const std::length_error&) {
            return std::unexpected(MinimizerGenerationWireError::admission);
        }

        std::uint64_t control_bytes = 0u;
        std::uint64_t position_bytes = 0u;
        std::uint64_t control_count = 0u;
        std::uint64_t position_count = 0u;
        std::uint64_t control_scalar_bytes = 0u;
        std::int32_t previous_segment = -1;
        for (std::size_t ordinal = 0u;
             ordinal < adapter.entries_.size(); ++ordinal) {
            const MinimizerBucketDirEntry& entry =
                adapter.entries_[ordinal];
            if (entry.position_segment < 0
                || static_cast<std::uint32_t>(entry.position_segment)
                    >= manifest.position_segment_count) {
                return std::unexpected(
                    MinimizerGenerationWireError::position_segment);
            }
            const auto layout = minimizer_shape_v5_control_layout(
                entry.control_element_count,
                profile->shape_v5->hash_width,
                entry.shape_offset_width,
                profile->shape_v5->count_width);
            std::uint64_t expected_position_bytes = 0u;
            std::uint64_t scalar_bytes_per_control = 0u;
            std::uint64_t entry_scalar_bytes = 0u;
            if (!layout) return std::unexpected(layout.error());
            if (layout->encoded_bytes != entry.control_encoded_bytes) {
                return std::unexpected(
                    MinimizerGenerationWireError::byte_count);
            }
            if (!checked_multiply_(entry.position_element_count,
                    static_cast<std::uint8_t>(
                        profile->shape_v5->position_width),
                    expected_position_bytes)
                || !checked_add_(
                    static_cast<std::uint8_t>(
                        profile->shape_v5->hash_width),
                    static_cast<std::uint8_t>(
                        profile->shape_v5->count_width),
                    scalar_bytes_per_control)
                || !checked_add_(scalar_bytes_per_control,
                    2u * static_cast<std::uint8_t>(
                        entry.shape_offset_width),
                    scalar_bytes_per_control)
                || !checked_multiply_(entry.control_element_count,
                    scalar_bytes_per_control, entry_scalar_bytes)) {
                return std::unexpected(
                    MinimizerGenerationWireError::arithmetic_overflow);
            }
            if (expected_position_bytes
                    != entry.position_encoded_bytes) {
                return std::unexpected(
                    MinimizerGenerationWireError::byte_count);
            }
            if (entry.position_segment != previous_segment) {
                const auto core = minimizer_position_segment_core(
                    manifest,
                    static_cast<std::uint32_t>(entry.position_segment));
                if (!core) return std::unexpected(core.error());
                try {
                    adapter.fragments_.push_back(FragmentPlan{
                        .entry_begin = ordinal,
                        .entry_end = ordinal + 1u,
                        .first_control_byte = control_bytes,
                        .first_position_relative_byte = position_bytes,
                        .core_begin_bp = core->begin_bp,
                        .core_end_bp = core->end_bp,
                    });
                } catch (const std::bad_alloc&) {
                    return std::unexpected(
                        MinimizerGenerationWireError::admission);
                } catch (const std::length_error&) {
                    return std::unexpected(
                        MinimizerGenerationWireError::admission);
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
                    MinimizerGenerationWireError::arithmetic_overflow);
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
                MinimizerGenerationWireError::element_count);
        }
        adapter.total_control_bytes_ = control_bytes;

        Format5CompactH32PrebuildEstimate estimate{
            .manifest_fragment_count = manifest.position_segment_count,
            .nonempty_fragment_count = adapter.fragments_.size(),
            .directory_entry_count = adapter.entries_.size(),
            .fragment_controls_per_source_scan = manifest.control_count,
            .control_scalar_bytes_per_source_scan = control_scalar_bytes,
            .fragment_plan_owner_capacity_bytes = saturating_multiply_(
                adapter.fragments_.capacity(), sizeof(FragmentPlan)),
            .source_token_owner_bytes = sizeof(SourceToken),
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
                adapter.fragments_.size(), sizeof(FragmentCursor)),
            .merge_heap_scratch_bytes = saturating_multiply_(
                adapter.fragments_.size(), sizeof(HeapNode)),
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
            adapter.source_token_ = std::make_unique<SourceToken>();
        } catch (const std::bad_alloc&) {
            return std::unexpected(MinimizerGenerationWireError::admission);
        }
        adapter.source_token_->owner = &adapter;
        adapter.ready_ = true;
        return std::move(adapter);
    }

    /// The returned source borrows this adapter, its `DirView`, and its mapped
    /// payload owner through both compact-index replays. Adapter moves are safe
    /// because dispatch uses a heap-stable token; destruction is invalidation.
    [[nodiscard]] CompactH32SortedRunSourceRef sorted_run_source() noexcept {
        return CompactH32SortedRunSourceRef{
            .context = ready_ && source_token_ != nullptr
                ? static_cast<void*>(source_token_.get()) : nullptr,
            .scan_sorted_runs_fn = &scan_thunk_,
        };
    }

    [[nodiscard]] std::expected<CompactH32BuildOptions,
        DirectInvertedIndexError>
    compact_build_options(
            std::uint32_t persisted_max_occ_cap,
            std::uint64_t max_image_bytes =
                (std::numeric_limits<std::uint64_t>::max)()) const noexcept {
        if (!ready_ || persisted_max_occ_cap == 0u
            || persisted_max_occ_cap > profile_.shape_v5->max_occ_cap) {
            return std::unexpected(
                DirectInvertedIndexError::compatibility);
        }
        return CompactH32BuildOptions{
            .identity = DirectIndexIdentity{
                .generation_id = head_.generation_id,
                .source_identity = head_.source_identity,
                .compatibility_checksum = head_.compatibility_checksum,
                .target_length = manifest_.target_length,
            },
            .max_occ_cap = persisted_max_occ_cap,
            .geometry = geometry_,
            .max_image_bytes = max_image_bytes,
        };
    }

    [[nodiscard]] bool ready() const noexcept { return ready_; }

    [[nodiscard]] const Format5CompactH32PrebuildEstimate&
    prebuild_estimate() const noexcept { return estimate_; }

    [[nodiscard]] const Format5CompactH32ScanEvidence&
    scan_evidence() const noexcept { return evidence_; }

    [[nodiscard]] CompactH32Geometry geometry() const noexcept {
        return geometry_;
    }
};

} // namespace bioseq::indexing
