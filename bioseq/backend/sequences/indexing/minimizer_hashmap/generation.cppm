/**
 * @file minimizer_hashmap/generation.cppm
 * @brief Grouped-SoA generation planning, emission, and validation.
 */
export module bioseq.indexing.minimizer_hashmap:generation;

import std;
import :exact_simd;
import :grouped_soa;
import :prepared_reader;
import :compact_direct;
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

/// Grouped-SoA generation controls.
/// Fixed-width limits of persisted controls and PostgreSQL key axes.
struct grouped_soa_generation_limits {
    std::uint64_t max_segment_control_count = (std::numeric_limits<std::uint32_t>::max)();
    std::uint64_t max_segment_position_count = (std::numeric_limits<std::uint32_t>::max)();
    std::uint64_t max_hash_position_count = (std::numeric_limits<std::uint32_t>::max)();
};

/// Immutable storage contract and source identity supplied to both producers.
struct grouped_soa_generation_options {
    minimizer_generation_layout layout = minimizer_generation_layout::invalid;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t source_content_checksum = 0;
    std::uint64_t target_length = 0;
    /// Requested persisted position geometry. Zero keeps the generic
    /// sequence-wide producer default. minimizer-fragment supplies its physical fragment
    /// span so one persisted segment remains one independently queryable
    /// source-fragment index. Minimizer-sequence keeps the larger merge-oriented default.
    std::uint64_t position_segment_span_bp = 0;
    std::uint16_t alphabet_code = 0;
    std::uint16_t storage_unit_byte_width = 0;
    std::uint16_t kmer_size = 0;
    std::uint16_t window_size = 0;
    // Deliberately invalid: every writer must name the producer's scheme.
    std::uint16_t minimizer_scheme_id = k_invalid_minimizer_scheme_id;
    std::uint16_t minimizer_scheme_revision = k_invalid_minimizer_scheme_revision;
    /// Grouped-SoA count saturation threshold.
    std::uint64_t max_occ_cap = k_grouped_soa_default_max_occurrence_cap;
};

/// One canonical event supplied in `(segment,hash,encoded_position)` order.
struct canonical_minimizer_event {
    std::uint64_t hash = 0;
    std::uint64_t encoded_position = 0;

    [[nodiscard]] bool operator==(const canonical_minimizer_event&) const = default;
};

/// Type-erased callback used by independently replayable minimizer-fragment/minimizer-sequence
/// producers.
struct canonical_minimizer_event_receiver {
    void* context = nullptr;
    std::expected<void, minimizer_generation_encoding_error> (*emit_fn)(
        void*, const canonical_minimizer_event&) = nullptr;

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    operator()(const canonical_minimizer_event& event) const {
        if (context == nullptr || emit_fn == nullptr) {
            return std::unexpected(minimizer_generation_encoding_error::row_count);
        }
        return emit_fn(context, event);
    }
};

/// A producer that can replay a bounded/external sorted stream for each pass.
template <class p_producer>
concept grouped_soa_event_producer = requires(p_producer& producer,
                                              std::uint64_t position_segment_span_bp,
                                              canonical_minimizer_event_receiver receiver) {
    { producer.scheme_identity() } -> std::same_as<minimizer_scheme_identity>;
    { producer.producer_backend_identity() } -> std::same_as<minimizer_producer_identity>;
    {
        producer.scan_sorted_events(position_segment_span_bp, receiver)
    } -> std::same_as<std::expected<void, minimizer_generation_encoding_error>>;
};

/// A sink that accepts authenticated generation rows in publication order.
template <class p_sink>
concept minimizer_generation_row_sink =
    requires(p_sink& sink, const minimizer_generation_row_view& row) {
        {
            sink.write(row)
        } -> std::same_as<std::expected<void, minimizer_generation_encoding_error>>;
    };

/// Immutable byte/layout contract offered before the first generation row is
/// emitted.  It deliberately contains no planner-owned spans: an optional
/// direct sink may retain this value while the common encoder remains the
/// authority for the eventual directory and manifest bytes.
struct grouped_soa_storage_plan_view {
    minimizer_generation_layout layout = minimizer_generation_layout::invalid;
    std::uint64_t generation_id = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t directory_bytes = 0;
    std::uint64_t manifest_bytes = 0;
    std::uint64_t directory_offset = 0;
    std::uint64_t manifest_offset = 0;
    std::uint64_t file_bytes = 0;

    [[nodiscard]] constexpr bool
    operator==(const grouped_soa_storage_plan_view&) const noexcept = default;
};

/// A generation sink that accepts the immutable whole-image storage plan.
template <class p_sink>
concept planned_grouped_soa_generation_sink =
    requires(p_sink& sink, const grouped_soa_storage_plan_view& storage_plan) {
        {
            sink.begin_planned_generation(storage_plan)
        } -> std::same_as<std::expected<void, minimizer_generation_encoding_error>>;
    };

/// grouped-SoA payload handoff before the address-bound row CRC exists. A sink
/// which implements the optional ownership method below becomes the sole CRC
/// calculator. Non-PostgreSQL sinks use the checked adapter fallback.
struct unchecked_minimizer_generation_row_view {
    index_chunk_address address{};
    std::span<const std::byte> bytes;
};

/// Authoritative checksum returned by the sink which accepted one grouped-SoA
/// payload row. CRC zero is valid, so success is represented by expected rather
/// than a sentinel value.
struct minimizer_generation_row_write_receipt {
    std::uint64_t row_checksum = 0;
};

/// A sink that owns grouped-SoA payload-row checksum calculation.
template <class p_sink>
concept grouped_soa_minimizer_payload_sink = requires(
    p_sink& sink, const unchecked_minimizer_generation_row_view& row) {
    {
        sink.write_grouped_soa_payload(row)
    } -> std::same_as<
        std::expected<minimizer_generation_row_write_receipt, minimizer_generation_encoding_error>>;
};

/// Type-erased generation sink with an optional payload-ownership callback.
struct minimizer_generation_row_sink_ref {
    void* context = nullptr;
    std::expected<void, minimizer_generation_encoding_error> (*write_fn)(
        void*, const minimizer_generation_row_view&) = nullptr;
    std::expected<minimizer_generation_row_write_receipt, minimizer_generation_encoding_error> (
        *write_grouped_soa_payload_fn)(void*,
                                       const unchecked_minimizer_generation_row_view&) = nullptr;

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    write(const minimizer_generation_row_view& row) const {
        if (context == nullptr || write_fn == nullptr) {
            return std::unexpected(minimizer_generation_encoding_error::row_count);
        }
        return write_fn(context, row);
    }

    /// Let an ownership-aware sink compute the grouped-SoA row CRC exactly once.
    /// Non-PostgreSQL sinks compute the checksum here and receive the same
    /// checksummed row API.
    [[nodiscard]] std::expected<minimizer_generation_row_write_receipt,
                                minimizer_generation_encoding_error>
    write_grouped_soa_payload(const unchecked_minimizer_generation_row_view& row) const {
        if (context == nullptr || write_fn == nullptr) {
            return std::unexpected(minimizer_generation_encoding_error::row_count);
        }
        if (write_grouped_soa_payload_fn != nullptr) {
            return write_grouped_soa_payload_fn(context, row);
        }
        const std::uint64_t row_checksum =
            crc64_index_chunk_row(row.address.generation_id,
                                  row.address.position_segment,
                                  row.address.sub_key,
                                  row.address.chunk_no,
                                  static_cast<std::uint32_t>(row.bytes.size()),
                                  row.bytes);
        if (auto written = write(minimizer_generation_row_view{
                .address = row.address,
                .bytes = row.bytes,
                .row_checksum = row_checksum,
            });
            !written) {
            return std::unexpected(written.error());
        }
        return minimizer_generation_row_write_receipt{
            .row_checksum = row_checksum,
        };
    }
};

/// One bounded fixed metadata row (head or manifest chunk zero).
struct encoded_minimizer_fixed_row {
    index_chunk_address address{};
    std::array<std::byte, minimizer_generation_manifest::encoded_bytes> storage{};
    std::uint32_t byte_count = 0;
    std::uint64_t row_checksum = 0;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return std::span<const std::byte>{storage.data(), byte_count};
    }
};

/// Bounded metadata result. Biological payload rows are never retained here.
struct encoded_grouped_soa_generation {
    minimizer_generation_head head{};
    minimizer_generation_manifest manifest{};
    /// Publication is deliberately separate: this row is never sent to the
    /// generation sink by `encode_minimizer_generation`.
    encoded_minimizer_fixed_row unpublished_head_row{};
    encoded_minimizer_fixed_row manifest_row{};
    grouped_soa_directory_view directory{};
};

/// Immutable identity of the exact encoded generation whose execution stats
/// may be admitted to READY publication.  Route stats alone is deliberately
/// insufficient: two generations can execute the same backend over equal-sized
/// sources while naming different source epochs or publication rows.
struct minimizer_generation_publication_binding {
    minimizer_generation_layout layout = minimizer_generation_layout::invalid;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t source_content_checksum = 0;
    std::uint64_t storage_contract_checksum = 0;
    std::uint64_t manifest_checksum = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return layout != minimizer_generation_layout::invalid && generation_id != 0 &&
               source_identity != 0;
    }

    [[nodiscard]] constexpr bool
    operator==(const minimizer_generation_publication_binding&) const noexcept = default;
};

/// Derive the exact publication identity from encoded metadata.
[[nodiscard]] constexpr minimizer_generation_publication_binding
make_minimizer_generation_publication_binding(
    const encoded_grouped_soa_generation& generation) noexcept {
    return minimizer_generation_publication_binding{
        .layout = generation.manifest.layout,
        .generation_id = generation.manifest.generation_id,
        .source_identity = generation.manifest.source_identity,
        .source_content_checksum = generation.manifest.source_content_checksum,
        .storage_contract_checksum =
            compute_minimizer_generation_storage_contract_checksum(generation.manifest),
        .manifest_checksum = generation.head.manifest_checksum,
    };
}

/// Replay-stable grouped-SoA directory and canonical event identity.
struct grouped_soa_generation_plan {
    grouped_soa_minimizer_profile profile{};
    minimizer_producer_identity producer_backend{};
    std::uint64_t position_segment_span_bp = 0;
    std::uint32_t position_segment_count = 0;
    std::uint64_t canonical_event_count = 0;
    std::uint64_t canonical_event_checksum = 0;
    std::vector<grouped_soa_bucket_directory_entry> entries;
};

namespace minimizer_generation_stream_detail {

/// Reserved bytes before the first planned payload section.
inline constexpr std::uint64_t k_planned_generation_image_header_bytes = 4096u;
/// Alignment shared by planned directory and manifest sections.
inline constexpr std::uint64_t k_planned_generation_section_alignment = 8u;

/// Checked addition used by whole-image storage planning.
[[nodiscard]] constexpr std::expected<std::uint64_t, minimizer_generation_encoding_error>
planned_storage_checked_add(std::uint64_t left, std::uint64_t right) noexcept {
    if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    return left + right;
}

/// Align one whole-image offset to the fixed section boundary.
[[nodiscard]] constexpr std::expected<std::uint64_t, minimizer_generation_encoding_error>
planned_storage_align_up(std::uint64_t value) noexcept {
    const std::uint64_t remainder = value % k_planned_generation_section_alignment;
    return planned_storage_checked_add(
        value, remainder == 0u ? 0u : k_planned_generation_section_alignment - remainder);
}

/// Build the immutable byte layout offered to a planned generation sink.
[[nodiscard]] inline std::expected<grouped_soa_storage_plan_view,
                                   minimizer_generation_encoding_error>
make_minimizer_generation_storage_plan(const grouped_soa_generation_options& options,
                                       const grouped_soa_generation_plan& plan) noexcept {
    if (options.layout == minimizer_generation_layout::invalid ||
        plan.profile.layout != options.layout) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }

    std::uint64_t payload_bytes = 0u;
    for (const grouped_soa_bucket_directory_entry& entry : plan.entries) {
        const auto entry_bytes =
            planned_storage_checked_add(entry.control_encoded_bytes, entry.position_encoded_bytes);
        if (!entry_bytes) {
            return std::unexpected(entry_bytes.error());
        }
        const auto next_payload_bytes = planned_storage_checked_add(payload_bytes, *entry_bytes);
        if (!next_payload_bytes) {
            return std::unexpected(next_payload_bytes.error());
        }
        payload_bytes = *next_payload_bytes;
    }

    const std::size_t directory_entry_bytes = minimizer_directory_entry_encoded_bytes(plan.profile);
    const std::size_t manifest_encoded_bytes =
        minimizer_manifest_encoded_bytes(plan.profile.layout);
    if (directory_entry_bytes == 0u || manifest_encoded_bytes == 0u ||
        !std::in_range<std::uint64_t>(plan.entries.size()) ||
        !std::in_range<std::uint64_t>(directory_entry_bytes) ||
        !std::in_range<std::uint64_t>(manifest_encoded_bytes)) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    const std::uint64_t entry_count = static_cast<std::uint64_t>(plan.entries.size());
    const std::uint64_t directory_entry_bytes_u64 =
        static_cast<std::uint64_t>(directory_entry_bytes);
    if (entry_count != 0u &&
        directory_entry_bytes_u64 > (std::numeric_limits<std::uint64_t>::max)() / entry_count) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    const std::uint64_t directory_bytes = entry_count * directory_entry_bytes_u64;
    const std::uint64_t manifest_bytes = static_cast<std::uint64_t>(manifest_encoded_bytes);

    const auto payload_end =
        planned_storage_checked_add(k_planned_generation_image_header_bytes, payload_bytes);
    if (!payload_end) {
        return std::unexpected(payload_end.error());
    }
    const auto directory_offset = planned_storage_align_up(*payload_end);
    if (!directory_offset) {
        return std::unexpected(directory_offset.error());
    }
    const auto directory_end = planned_storage_checked_add(*directory_offset, directory_bytes);
    if (!directory_end) {
        return std::unexpected(directory_end.error());
    }
    const auto manifest_offset = planned_storage_align_up(*directory_end);
    if (!manifest_offset) {
        return std::unexpected(manifest_offset.error());
    }
    const auto file_bytes = planned_storage_checked_add(*manifest_offset, manifest_bytes);
    if (!file_bytes) {
        return std::unexpected(file_bytes.error());
    }
    return grouped_soa_storage_plan_view{
        .layout = options.layout,
        .generation_id = options.generation_id,
        .payload_bytes = payload_bytes,
        .directory_bytes = directory_bytes,
        .manifest_bytes = manifest_bytes,
        .directory_offset = *directory_offset,
        .manifest_offset = *manifest_offset,
        .file_bytes = *file_bytes,
    };
}

/// Fold one canonical event into the replay checksum in fixed wire order.
inline void update_canonical_event_checksum(crc64_ecma182_accumulator& checksum,
                                            const canonical_minimizer_event& event) noexcept {
    std::array<std::byte, 16> encoded{};
    std::span<std::byte> bytes{encoded};
    minimizer_generation_wire_detail::write_le<std::uint64_t>(bytes, 0, event.hash);
    minimizer_generation_wire_detail::write_le<std::uint64_t>(bytes, 8, event.encoded_position);
    checksum.update(encoded);
}

/// Return whether a profile is valid for grouped-SoA payload emission.
[[nodiscard]] constexpr bool
is_grouped_soa_payload_profile(const grouped_soa_minimizer_profile& profile) noexcept {
    return profile.layout == minimizer_generation_layout::grouped_soa_segment_local &&
           validate_grouped_soa_minimizer_profile(profile).has_value();
}

/// Compute a checked fixed-width payload byte count.
[[nodiscard]] constexpr std::expected<std::uint64_t, minimizer_generation_encoding_error>
checked_payload_byte_count(std::uint64_t element_count, std::uint64_t encoded_byte_width) noexcept {
    if (encoded_byte_width == 0 ||
        element_count > (std::numeric_limits<std::uint64_t>::max)() / encoded_byte_width) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    return element_count * encoded_byte_width;
}

/// Return the number of fixed payload rows needed for an encoded extent.
[[nodiscard]] constexpr std::uint64_t
grouped_soa_payload_chunk_count(std::uint64_t encoded_bytes) noexcept {
    constexpr std::uint32_t extent_payload_bytes = k_grouped_soa_extent_payload_bytes;
    static_assert(extent_payload_bytes != 0);
    return encoded_bytes / extent_payload_bytes +
           (encoded_bytes % extent_payload_bytes != 0 ? 1u : 0u);
}

/// grouped-SoA planning keeps the terminal prefix authoritative and derives the
/// per-entry offset width before committing an extent. A hash group is indivisible:
/// an ordinary entry is flushed before a widening group, while a heavy group
/// owns the resulting wider entry by itself.
struct grouped_soa_plan_pass {
    std::uint64_t span = 0;
    std::uint64_t target_length = 0;
    grouped_soa_minimizer_profile profile{};
    grouped_soa_generation_limits limits{};
    std::vector<grouped_soa_bucket_directory_entry> entries;
    bool retry_with_smaller_span = false;
    bool has_event = false;
    std::uint64_t previous_segment = 0;
    canonical_minimizer_event previous_event{};
    std::uint64_t current_segment = 0;
    std::uint64_t current_hash = 0;
    std::uint64_t group_positions = 0;
    std::uint64_t segment_controls = 0;
    std::uint64_t segment_positions = 0;
    std::uint64_t partition_controls = 0;
    std::uint64_t partition_positions = 0;
    std::uint64_t partition_lo_hash = 0;
    std::uint64_t partition_hi_hash = 0;
    std::uint64_t canonical_event_count = 0;
    crc64_ecma182_accumulator canonical_event_checksum{};

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error> flush_partition() {
        if (partition_controls == 0)
            return {};
        if (!sql_integer_can_represent(current_segment) ||
            partition_controls > (std::numeric_limits<std::uint32_t>::max)() ||
            entries.size() >= (std::numeric_limits<std::uint32_t>::max)() / 2u) {
            return std::unexpected(minimizer_generation_encoding_error::element_count);
        }
        const minimizer_scalar_width offset_width =
            select_grouped_soa_offset_width(partition_positions);
        const auto layout = plan_grouped_soa_control_plane(
            partition_controls, profile.hash_width, offset_width, profile.count_width);
        const auto position_bytes = checked_payload_byte_count(
            partition_positions, static_cast<std::uint8_t>(profile.position_width));
        if (!layout || !position_bytes) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        const std::uint64_t control_chunks = grouped_soa_payload_chunk_count(layout->encoded_bytes);
        const std::uint64_t position_chunks = grouped_soa_payload_chunk_count(*position_bytes);
        if (control_chunks > (std::numeric_limits<std::uint32_t>::max)() ||
            position_chunks > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::chunk_range);
        }
        const std::uint32_t partition_id = static_cast<std::uint32_t>(entries.size());
        entries.push_back(grouped_soa_bucket_directory_entry{
            .partition_id = partition_id,
            .position_segment = static_cast<std::int32_t>(current_segment),
            .lo_hash = partition_lo_hash,
            .hi_hash = partition_hi_hash,
            .sub_key = static_cast<index_sub_key_t>(partition_id),
            .control_first_chunk = 0,
            .control_chunk_count = static_cast<std::uint32_t>(control_chunks),
            .position_first_chunk = 0,
            .position_chunk_count = static_cast<std::uint32_t>(position_chunks),
            .control_element_count = partition_controls,
            .position_element_count = partition_positions,
            .control_encoded_bytes = layout->encoded_bytes,
            .position_encoded_bytes = *position_bytes,
            .shape_offset_width = offset_width,
        });
        partition_controls = 0;
        partition_positions = 0;
        partition_lo_hash = 0;
        partition_hi_hash = 0;
        return {};
    }

    [[nodiscard]] std::expected<std::uint64_t, minimizer_generation_encoding_error>
    prospective_partition_bytes(std::uint64_t controls, std::uint64_t positions) const noexcept {
        const auto layout =
            plan_grouped_soa_control_plane(controls,
                                           profile.hash_width,
                                           select_grouped_soa_offset_width(positions),
                                           profile.count_width);
        const auto position_bytes = checked_payload_byte_count(
            positions, static_cast<std::uint8_t>(profile.position_width));
        if (!layout || !position_bytes ||
            layout->encoded_bytes > (std::numeric_limits<std::uint64_t>::max)() - *position_bytes) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        return layout->encoded_bytes + *position_bytes;
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error> finish_group() {
        if (group_positions == 0)
            return {};
        if (partition_positions > (std::numeric_limits<std::uint64_t>::max)() - group_positions) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        const auto prospective = prospective_partition_bytes(partition_controls + 1u,
                                                             partition_positions + group_positions);
        if (!prospective)
            return std::unexpected(prospective.error());
        if (partition_controls != 0 &&
            (*prospective > k_minimizer_partition_target_bytes ||
             select_grouped_soa_offset_width(partition_positions) !=
                 select_grouped_soa_offset_width(partition_positions + group_positions))) {
            if (auto flushed = flush_partition(); !flushed)
                return flushed;
        }
        if (partition_controls == 0)
            partition_lo_hash = current_hash;
        partition_hi_hash = current_hash;
        ++partition_controls;
        partition_positions += group_positions;
        group_positions = 0;
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    accept(const canonical_minimizer_event& event) {
        const std::uint64_t position = event.encoded_position >> 1u;
        if (span == 0 || position >= target_length || event.hash > profile.max_hash) {
            return std::unexpected(event.hash > profile.max_hash
                                       ? minimizer_generation_encoding_error::hash_range
                                       : minimizer_generation_encoding_error::position_range);
        }
        const std::uint64_t segment = position / span;
        if (!sql_integer_can_represent(segment)) {
            return std::unexpected(minimizer_generation_encoding_error::position_range);
        }
        if (has_event) {
            const bool before = segment < previous_segment ||
                                (segment == previous_segment &&
                                 (event.hash < previous_event.hash ||
                                  (event.hash == previous_event.hash &&
                                   event.encoded_position < previous_event.encoded_position)));
            if (before) {
                return std::unexpected(minimizer_generation_encoding_error::row_order);
            }
            if (segment == previous_segment && event == previous_event) {
                return std::unexpected(minimizer_generation_encoding_error::duplicate_event);
            }
            if (segment != current_segment || event.hash != current_hash) {
                if (auto group = finish_group(); !group)
                    return group;
                if (segment != current_segment) {
                    if (auto flushed = flush_partition(); !flushed) {
                        return flushed;
                    }
                    current_segment = segment;
                    segment_controls = 0;
                    segment_positions = 0;
                }
                current_hash = event.hash;
                ++segment_controls;
            }
        } else {
            has_event = true;
            current_segment = segment;
            current_hash = event.hash;
            segment_controls = 1;
        }
        if (canonical_event_count == (std::numeric_limits<std::uint64_t>::max)() ||
            segment_positions == (std::numeric_limits<std::uint64_t>::max)() ||
            group_positions == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        ++canonical_event_count;
        update_canonical_event_checksum(canonical_event_checksum, event);
        ++segment_positions;
        ++group_positions;
        if (segment_controls > limits.max_segment_control_count ||
            segment_positions > limits.max_segment_position_count ||
            group_positions > limits.max_hash_position_count) {
            retry_with_smaller_span = true;
            return std::unexpected(minimizer_generation_encoding_error::element_count);
        }
        previous_segment = segment;
        previous_event = event;
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error> finish() {
        if (auto group = finish_group(); !group)
            return group;
        return flush_partition();
    }
};

[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
receive_grouped_soa_plan_event(void* context, const canonical_minimizer_event& event) {
    return static_cast<grouped_soa_plan_pass*>(context)->accept(event);
}

template <grouped_soa_event_producer p_producer>
[[nodiscard]] inline bool producer_identity_matches(const p_producer& producer,
                                                    minimizer_scheme_identity scheme,
                                                    minimizer_producer_identity backend) noexcept {
    return producer.scheme_identity() == scheme && producer.producer_backend_identity() == backend;
}

/// Bounded writer shared by the control and position replay passes.  It owns
/// exactly one physical row, streams extent bytes into fixed 8064-byte chunks,
/// and folds the sink-authoritative row CRC receipts into the generation
/// payload checksum in write order.
struct grouped_soa_payload_extent_writer {
    minimizer_generation_row_sink_ref sink{};
    crc64_ecma182_accumulator* payload_row_checksums = nullptr;
    index_chunk_address first_address{};
    std::uint64_t expected_bytes = 0u;
    std::uint32_t expected_chunks = 0u;
    std::array<std::byte, k_grouped_soa_extent_payload_bytes> row_bytes{};
    std::size_t buffered_bytes = 0u;
    std::uint64_t written_bytes = 0u;
    std::uint32_t written_chunks = 0u;
    crc64_ecma182_accumulator extent_checksum{};
    bool active = false;

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    begin(minimizer_generation_row_sink_ref row_sink,
          crc64_ecma182_accumulator& generation_payload_checksums,
          index_chunk_address address,
          std::uint64_t byte_count,
          std::uint32_t chunk_count) noexcept {
        if (active || row_sink.context == nullptr || row_sink.write_fn == nullptr ||
            byte_count == 0u || chunk_count == 0u ||
            grouped_soa_payload_chunk_count(byte_count) != chunk_count) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        sink = row_sink;
        payload_row_checksums = &generation_payload_checksums;
        first_address = address;
        expected_bytes = byte_count;
        expected_chunks = chunk_count;
        buffered_bytes = 0u;
        written_bytes = 0u;
        written_chunks = 0u;
        extent_checksum = {};
        active = true;
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error> flush() {
        if (!active || payload_row_checksums == nullptr) {
            return std::unexpected(minimizer_generation_encoding_error::row_order);
        }
        if (buffered_bytes == 0u)
            return {};
        if (written_chunks >= expected_chunks || written_bytes > expected_bytes ||
            buffered_bytes > expected_bytes - written_bytes ||
            first_address.chunk_no > (std::numeric_limits<std::uint32_t>::max)() - written_chunks) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        index_chunk_address address = first_address;
        address.chunk_no += written_chunks;
        const std::span<const std::byte> bytes{row_bytes.data(), buffered_bytes};
        const auto receipt = sink.write_grouped_soa_payload(unchecked_minimizer_generation_row_view{
            .address = address,
            .bytes = bytes,
        });
        if (!receipt)
            return std::unexpected(receipt.error());
        payload_row_checksums->update_little_endian(receipt->row_checksum);
        written_bytes += buffered_bytes;
        ++written_chunks;
        buffered_bytes = 0u;
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    append(std::span<const std::byte> bytes) {
        if (!active || written_bytes > expected_bytes ||
            buffered_bytes > expected_bytes - written_bytes ||
            bytes.size() > expected_bytes - written_bytes - buffered_bytes) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        extent_checksum.update(bytes);
        while (!bytes.empty()) {
            const std::size_t copied = std::min(bytes.size(), row_bytes.size() - buffered_bytes);
            std::copy_n(bytes.begin(), copied, row_bytes.begin() + buffered_bytes);
            buffered_bytes += copied;
            bytes = bytes.subspan(copied);
            if (buffered_bytes == row_bytes.size()) {
                if (auto flushed = flush(); !flushed)
                    return flushed;
            }
        }
        return {};
    }

    [[nodiscard]] std::expected<std::uint64_t, minimizer_generation_encoding_error> finish() {
        if (auto flushed = flush(); !flushed) {
            return std::unexpected(flushed.error());
        }
        if (written_bytes != expected_bytes || written_chunks != expected_chunks) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        active = false;
        return extent_checksum.checksum();
    }
};

/// Replay pass which materializes one planned control extent at a time.  The
/// entry image is bounded by the planner's partition target. Positions remain
/// count-only in this pass.  Zero initialization makes every alignment suffix
/// part of the persisted corruption contract.
struct grouped_soa_control_emit_pass {
    grouped_soa_generation_options options{};
    grouped_soa_minimizer_profile profile{};
    std::uint64_t span = 0u;
    std::vector<grouped_soa_bucket_directory_entry>* entries = nullptr;
    minimizer_generation_row_sink_ref sink{};
    crc64_ecma182_accumulator* payload_row_checksums = nullptr;
    std::uint64_t canonical_event_count = 0u;
    crc64_ecma182_accumulator canonical_event_checksum{};
    std::size_t entry_ordinal = 0u;
    std::vector<std::byte> control_image{};
    std::optional<grouped_soa_control_plane_offsets> control_layout{};
    canonical_minimizer_event previous_event{};
    std::uint64_t previous_segment = 0u;
    std::uint64_t current_hash = 0u;
    std::uint64_t entry_control_count = 0u;
    std::uint64_t entry_position_count = 0u;
    std::uint64_t group_position_count = 0u;
    bool has_event = false;
    bool entry_open = false;
    bool group_open = false;

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    validate_contract() const noexcept {
        if (entries == nullptr || payload_row_checksums == nullptr || sink.context == nullptr ||
            sink.write_fn == nullptr || options.layout != profile.layout ||
            !is_grouped_soa_payload_profile(profile) ||
            ((!entries->empty() || options.target_length != 0u) && span == 0u)) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    open_entry(std::uint64_t segment, std::uint64_t hash) {
        if (entry_open || entries == nullptr || entry_ordinal >= entries->size()) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        grouped_soa_bucket_directory_entry& entry = (*entries)[entry_ordinal];
        if (const auto error = validate_minimizer_bucket_dir_entry(entry, profile)) {
            return std::unexpected(*error);
        }
        const auto layout = plan_grouped_soa_control_plane(entry.control_element_count,
                                                           profile.hash_width,
                                                           entry.shape_offset_width,
                                                           profile.count_width);
        if (!layout || layout->encoded_bytes != entry.control_encoded_bytes ||
            entry.control_encoded_bytes > (std::numeric_limits<std::size_t>::max)() ||
            segment != static_cast<std::uint64_t>(entry.position_segment) ||
            hash != entry.lo_hash) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        control_image.assign(static_cast<std::size_t>(entry.control_encoded_bytes), std::byte{0});
        control_layout = *layout;
        entry_control_count = 0u;
        entry_position_count = 0u;
        group_position_count = 0u;
        group_open = false;
        entry_open = true;
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error> finish_group() noexcept {
        if (!group_open)
            return {};
        if (!entry_open || entries == nullptr || !control_layout ||
            entry_ordinal >= entries->size() || group_position_count == 0u) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        const grouped_soa_bucket_directory_entry& entry = (*entries)[entry_ordinal];
        if (entry_control_count >= entry.control_element_count ||
            entry_position_count < group_position_count) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        const std::size_t hash_width = static_cast<std::uint8_t>(profile.hash_width);
        const std::size_t offset_width = static_cast<std::uint8_t>(entry.shape_offset_width);
        const std::size_t count_width = static_cast<std::uint8_t>(profile.count_width);
        const std::uint64_t prefix = entry_position_count - group_position_count;
        const std::uint64_t saturated_count =
            std::min(group_position_count, profile.max_occ_cap + 1u);
        std::span<std::byte> bytes{control_image};
        if (!minimizer_generation_wire_detail::write_shape_scalar(
                bytes,
                static_cast<std::size_t>(entry_control_count) * hash_width,
                profile.hash_width,
                current_hash) ||
            !minimizer_generation_wire_detail::write_shape_scalar(
                bytes,
                static_cast<std::size_t>(control_layout->prefix_offset) +
                    static_cast<std::size_t>(entry_control_count) * offset_width,
                entry.shape_offset_width,
                prefix) ||
            !minimizer_generation_wire_detail::write_shape_scalar(
                bytes,
                static_cast<std::size_t>(control_layout->count_offset) +
                    static_cast<std::size_t>(entry_control_count) * count_width,
                profile.count_width,
                saturated_count) ||
            !minimizer_generation_wire_detail::write_shape_scalar(
                bytes,
                static_cast<std::size_t>(control_layout->prefix_offset) +
                    static_cast<std::size_t>(entry_control_count + 1u) * offset_width,
                entry.shape_offset_width,
                entry_position_count)) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        ++entry_control_count;
        group_position_count = 0u;
        group_open = false;
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error> finish_entry() {
        if (!entry_open || entries == nullptr || entry_ordinal >= entries->size()) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        if (auto finished = finish_group(); !finished)
            return finished;
        grouped_soa_bucket_directory_entry& entry = (*entries)[entry_ordinal];
        if (entry_control_count != entry.control_element_count ||
            entry_position_count != entry.position_element_count || current_hash != entry.hi_hash ||
            control_image.size() != entry.control_encoded_bytes) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        grouped_soa_payload_extent_writer writer;
        if (auto begun = writer.begin(sink,
                                      *payload_row_checksums,
                                      index_chunk_address{
                                          .generation_id = options.generation_id,
                                          .position_segment = entry.position_segment,
                                          .sub_key = entry.sub_key,
                                          .chunk_no = entry.control_first_chunk,
                                      },
                                      entry.control_encoded_bytes,
                                      entry.control_chunk_count);
            !begun) {
            return begun;
        }
        if (auto appended = writer.append(control_image); !appended) {
            return appended;
        }
        const auto checksum = writer.finish();
        if (!checksum)
            return std::unexpected(checksum.error());
        entry.control_checksum = *checksum;
        ++entry_ordinal;
        entry_open = false;
        control_layout.reset();
        control_image.clear();
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    accept(const canonical_minimizer_event& event) {
        if (auto valid = validate_contract(); !valid)
            return valid;
        const std::uint64_t position = event.encoded_position >> 1u;
        if (position >= options.target_length || event.hash > profile.max_hash) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        const std::uint64_t segment = position / span;
        if (!sql_integer_can_represent(segment)) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        if (has_event) {
            const bool before = segment < previous_segment ||
                                (segment == previous_segment &&
                                 (event.hash < previous_event.hash ||
                                  (event.hash == previous_event.hash &&
                                   event.encoded_position < previous_event.encoded_position)));
            if (before || (segment == previous_segment && event == previous_event)) {
                return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
            }
        }
        if (!entry_open) {
            if (auto opened = open_entry(segment, event.hash); !opened) {
                return opened;
            }
        } else {
            const grouped_soa_bucket_directory_entry& entry = (*entries)[entry_ordinal];
            if (segment != static_cast<std::uint64_t>(entry.position_segment) ||
                event.hash > entry.hi_hash) {
                if (segment < static_cast<std::uint64_t>(entry.position_segment)) {
                    return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
                }
                if (auto finished = finish_entry(); !finished) {
                    return finished;
                }
                if (auto opened = open_entry(segment, event.hash); !opened) {
                    return opened;
                }
            } else if (event.hash < entry.lo_hash) {
                return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
            }
        }
        if (group_open && event.hash != current_hash) {
            if (auto finished = finish_group(); !finished)
                return finished;
        }
        if (!group_open) {
            if (entry_control_count != 0u && event.hash <= current_hash) {
                return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
            }
            current_hash = event.hash;
            group_open = true;
        }
        if (canonical_event_count == (std::numeric_limits<std::uint64_t>::max)() ||
            entry_position_count == (std::numeric_limits<std::uint64_t>::max)() ||
            group_position_count == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        ++canonical_event_count;
        update_canonical_event_checksum(canonical_event_checksum, event);
        ++entry_position_count;
        ++group_position_count;
        previous_segment = segment;
        previous_event = event;
        has_event = true;
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error> finish() {
        if (auto valid = validate_contract(); !valid)
            return valid;
        if (entry_open) {
            if (auto finished = finish_entry(); !finished)
                return finished;
        }
        if (entry_ordinal != entries->size() || (has_event != !entries->empty())) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        return {};
    }
};

/// Replay pass which streams segment-local positions directly into bounded
/// rows.  A heavy hash can therefore own an arbitrarily large planned extent
/// without creating a correspondingly large transient position buffer.
struct grouped_soa_position_emit_pass {
    grouped_soa_generation_options options{};
    grouped_soa_minimizer_profile profile{};
    std::uint64_t span = 0u;
    std::vector<grouped_soa_bucket_directory_entry>* entries = nullptr;
    minimizer_generation_row_sink_ref sink{};
    crc64_ecma182_accumulator* payload_row_checksums = nullptr;
    std::uint64_t canonical_event_count = 0u;
    crc64_ecma182_accumulator canonical_event_checksum{};
    std::size_t entry_ordinal = 0u;
    grouped_soa_payload_extent_writer writer{};
    canonical_minimizer_event previous_event{};
    std::uint64_t previous_segment = 0u;
    std::uint64_t current_hash = 0u;
    std::uint64_t entry_control_count = 0u;
    std::uint64_t entry_position_count = 0u;
    std::uint64_t group_position_count = 0u;
    bool has_event = false;
    bool entry_open = false;
    bool group_open = false;

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    validate_contract() const noexcept {
        if (entries == nullptr || payload_row_checksums == nullptr || sink.context == nullptr ||
            sink.write_fn == nullptr || options.layout != profile.layout ||
            !is_grouped_soa_payload_profile(profile) ||
            ((!entries->empty() || options.target_length != 0u) && span == 0u)) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    open_entry(std::uint64_t segment, std::uint64_t hash) {
        if (entry_open || entries == nullptr || entry_ordinal >= entries->size()) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        grouped_soa_bucket_directory_entry& entry = (*entries)[entry_ordinal];
        if (const auto error = validate_minimizer_bucket_dir_entry(entry, profile)) {
            return std::unexpected(*error);
        }
        const auto position_sub_key = derive_minimizer_position_extent_sub_key(entry.sub_key);
        if (!position_sub_key || segment != static_cast<std::uint64_t>(entry.position_segment) ||
            hash != entry.lo_hash) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        if (auto begun = writer.begin(sink,
                                      *payload_row_checksums,
                                      index_chunk_address{
                                          .generation_id = options.generation_id,
                                          .position_segment = entry.position_segment,
                                          .sub_key = *position_sub_key,
                                          .chunk_no = entry.position_first_chunk,
                                      },
                                      entry.position_encoded_bytes,
                                      entry.position_chunk_count);
            !begun) {
            return begun;
        }
        entry_control_count = 0u;
        entry_position_count = 0u;
        group_position_count = 0u;
        group_open = false;
        entry_open = true;
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error> finish_group() noexcept {
        if (!group_open)
            return {};
        if (!entry_open || entries == nullptr || entry_ordinal >= entries->size() ||
            group_position_count == 0u ||
            entry_control_count >= (*entries)[entry_ordinal].control_element_count) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        ++entry_control_count;
        group_position_count = 0u;
        group_open = false;
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error> finish_entry() {
        if (!entry_open || entries == nullptr || entry_ordinal >= entries->size()) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        if (auto finished = finish_group(); !finished)
            return finished;
        grouped_soa_bucket_directory_entry& entry = (*entries)[entry_ordinal];
        if (entry_control_count != entry.control_element_count ||
            entry_position_count != entry.position_element_count || current_hash != entry.hi_hash) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        const auto checksum = writer.finish();
        if (!checksum)
            return std::unexpected(checksum.error());
        entry.position_checksum = *checksum;
        ++entry_ordinal;
        entry_open = false;
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    accept(const canonical_minimizer_event& event) {
        if (auto valid = validate_contract(); !valid)
            return valid;
        const std::uint64_t position = event.encoded_position >> 1u;
        if (position >= options.target_length || event.hash > profile.max_hash) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        const std::uint64_t segment = position / span;
        if (!sql_integer_can_represent(segment)) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        if (has_event) {
            const bool before = segment < previous_segment ||
                                (segment == previous_segment &&
                                 (event.hash < previous_event.hash ||
                                  (event.hash == previous_event.hash &&
                                   event.encoded_position < previous_event.encoded_position)));
            if (before || (segment == previous_segment && event == previous_event)) {
                return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
            }
        }
        if (!entry_open) {
            if (auto opened = open_entry(segment, event.hash); !opened) {
                return opened;
            }
        } else {
            const grouped_soa_bucket_directory_entry& entry = (*entries)[entry_ordinal];
            if (segment != static_cast<std::uint64_t>(entry.position_segment) ||
                event.hash > entry.hi_hash) {
                if (segment < static_cast<std::uint64_t>(entry.position_segment)) {
                    return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
                }
                if (auto finished = finish_entry(); !finished) {
                    return finished;
                }
                if (auto opened = open_entry(segment, event.hash); !opened) {
                    return opened;
                }
            } else if (event.hash < entry.lo_hash) {
                return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
            }
        }
        if (group_open && event.hash != current_hash) {
            if (auto finished = finish_group(); !finished)
                return finished;
        }
        if (!group_open) {
            if (entry_control_count != 0u && event.hash <= current_hash) {
                return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
            }
            current_hash = event.hash;
            group_open = true;
        }
        const std::uint64_t local_bp = position % span;
        if (local_bp > ((std::numeric_limits<std::uint64_t>::max)() >> 1u)) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        const std::uint64_t local_position = (local_bp << 1u) | (event.encoded_position & 1u);
        std::array<std::byte, sizeof(std::uint64_t)> encoded_position{};
        const std::size_t position_width = static_cast<std::uint8_t>(profile.position_width);
        std::span<std::byte> encoded_bytes{encoded_position.data(), position_width};
        if (!minimizer_generation_wire_detail::write_shape_scalar(
                encoded_bytes, 0u, profile.position_width, local_position)) {
            return std::unexpected(minimizer_generation_encoding_error::position_range);
        }
        if (auto appended = writer.append(encoded_bytes); !appended) {
            return appended;
        }
        if (canonical_event_count == (std::numeric_limits<std::uint64_t>::max)() ||
            entry_position_count == (std::numeric_limits<std::uint64_t>::max)() ||
            group_position_count == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        ++canonical_event_count;
        update_canonical_event_checksum(canonical_event_checksum, event);
        ++entry_position_count;
        ++group_position_count;
        previous_segment = segment;
        previous_event = event;
        has_event = true;
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error> finish() {
        if (auto valid = validate_contract(); !valid)
            return valid;
        if (entry_open) {
            if (auto finished = finish_entry(); !finished)
                return finished;
        }
        if (entry_ordinal != entries->size() || (has_event != !entries->empty())) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        return {};
    }
};

struct grouped_soa_emit_pass {
    template <grouped_soa_event_producer p_producer>
    [[nodiscard]] static std::expected<std::uint64_t, minimizer_generation_encoding_error>
    run(p_producer& producer,
        const grouped_soa_generation_options& options,
        const grouped_soa_minimizer_profile& profile,
        grouped_soa_generation_plan& plan,
        const minimizer_generation_row_sink_ref& sink) {
        if (plan.profile != profile) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        crc64_ecma182_accumulator payload_row_checksums;
        grouped_soa_control_emit_pass controls{
            .options = options,
            .profile = profile,
            .span = plan.position_segment_span_bp,
            .entries = &plan.entries,
            .sink = sink,
            .payload_row_checksums = &payload_row_checksums,
        };
        if (!producer_identity_matches(producer, profile.minimizer_scheme, plan.producer_backend)) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        auto replay = producer.scan_sorted_events(
            plan.position_segment_span_bp == 0 ? 1u : plan.position_segment_span_bp,
            canonical_minimizer_event_receiver{
                .context = &controls,
                .emit_fn =
                    [](void* context, const canonical_minimizer_event& event) {
                        return static_cast<grouped_soa_control_emit_pass*>(context)->accept(event);
                    },
            });
        if (!producer_identity_matches(producer, profile.minimizer_scheme, plan.producer_backend)) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        if (!replay)
            return std::unexpected(replay.error());
        if (auto finished = controls.finish(); !finished) {
            return std::unexpected(finished.error());
        }
        if (controls.canonical_event_count != plan.canonical_event_count ||
            controls.canonical_event_checksum.checksum() != plan.canonical_event_checksum) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        if (producer.scheme_identity() != profile.minimizer_scheme) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }

        grouped_soa_position_emit_pass positions{
            .options = options,
            .profile = profile,
            .span = plan.position_segment_span_bp,
            .entries = &plan.entries,
            .sink = sink,
            .payload_row_checksums = &payload_row_checksums,
        };
        if (!producer_identity_matches(producer, profile.minimizer_scheme, plan.producer_backend)) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        replay = producer.scan_sorted_events(
            plan.position_segment_span_bp == 0 ? 1u : plan.position_segment_span_bp,
            canonical_minimizer_event_receiver{
                .context = &positions,
                .emit_fn =
                    [](void* context, const canonical_minimizer_event& event) {
                        return static_cast<grouped_soa_position_emit_pass*>(context)->accept(event);
                    },
            });
        if (!producer_identity_matches(producer, profile.minimizer_scheme, plan.producer_backend)) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        if (!replay)
            return std::unexpected(replay.error());
        if (auto finished = positions.finish(); !finished) {
            return std::unexpected(finished.error());
        }
        if (positions.canonical_event_count != plan.canonical_event_count ||
            positions.canonical_event_checksum.checksum() != plan.canonical_event_checksum) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        return payload_row_checksums.checksum();
    }
};

template <grouped_soa_event_producer p_producer>
[[nodiscard]] inline std::expected<grouped_soa_generation_plan, minimizer_generation_encoding_error>
plan_grouped_soa_minimizer_generation(p_producer& producer,
                                      const grouped_soa_generation_options& options,
                                      const grouped_soa_minimizer_profile& profile,
                                      minimizer_producer_identity planned_backend,
                                      const grouped_soa_generation_limits& limits) {
    if (!is_grouped_soa_payload_profile(profile)) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    if (options.target_length == 0) {
        const auto empty_profile = derive_grouped_soa_minimizer_profile(profile.minimizer_scheme,
                                                                        options.alphabet_code,
                                                                        profile.kmer_size,
                                                                        1u,
                                                                        options.max_occ_cap);
        if (!empty_profile) {
            return std::unexpected(empty_profile.error());
        }
        grouped_soa_plan_pass empty_pass{
            .span = 1,
            .target_length = 0,
            .profile = *empty_profile,
            .limits = limits,
        };
        if (!producer_identity_matches(
                producer, empty_profile->minimizer_scheme, planned_backend)) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        const auto scanned =
            producer.scan_sorted_events(1,
                                        canonical_minimizer_event_receiver{
                                            .context = &empty_pass,
                                            .emit_fn = receive_grouped_soa_plan_event,
                                        });
        if (!producer_identity_matches(
                producer, empty_profile->minimizer_scheme, planned_backend)) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        if (!scanned) {
            return std::unexpected(scanned.error());
        }
        if (empty_pass.has_event) {
            return std::unexpected(minimizer_generation_encoding_error::position_range);
        }
        return grouped_soa_generation_plan{
            .profile = *empty_profile,
            /* Empty generations have no physical position geometry. Replay
             * uses a non-zero sentinel while the manifest publishes zero. */
            .position_segment_span_bp = 0u,
        };
    }

    std::uint64_t span = options.position_segment_span_bp == 0u
                             ? k_minimizer_preferred_segment_span_bp
                             : options.position_segment_span_bp;
    for (;;) {
        const std::uint64_t segment_count =
            options.target_length / span + (options.target_length % span != 0 ? 1u : 0u);
        if (!sql_integer_can_represent(segment_count)) {
            return std::unexpected(minimizer_generation_encoding_error::segment_count);
        }
        const auto attempt_profile = derive_grouped_soa_minimizer_profile(profile.minimizer_scheme,
                                                                          options.alphabet_code,
                                                                          profile.kmer_size,
                                                                          span,
                                                                          options.max_occ_cap);
        if (!attempt_profile) {
            return std::unexpected(attempt_profile.error());
        }
        grouped_soa_plan_pass pass{
            .span = span,
            .target_length = options.target_length,
            .profile = *attempt_profile,
            .limits = limits,
        };
        if (!producer_identity_matches(
                producer, attempt_profile->minimizer_scheme, planned_backend)) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        auto scanned = producer.scan_sorted_events(span,
                                                   canonical_minimizer_event_receiver{
                                                       .context = &pass,
                                                       .emit_fn = receive_grouped_soa_plan_event,
                                                   });
        if (!producer_identity_matches(
                producer, attempt_profile->minimizer_scheme, planned_backend)) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
        if (scanned) {
            scanned = pass.finish();
        }
        if (scanned) {
            return grouped_soa_generation_plan{
                .profile = *attempt_profile,
                .position_segment_span_bp = span,
                .position_segment_count = static_cast<std::uint32_t>(segment_count),
                .canonical_event_count = pass.canonical_event_count,
                .canonical_event_checksum = pass.canonical_event_checksum.checksum(),
                .entries = std::move(pass.entries),
            };
        }
        if (!pass.retry_with_smaller_span || span == 1u) {
            return std::unexpected(scanned.error());
        }
        span /= 2u;
    }
}

template <grouped_soa_event_producer p_producer>
[[nodiscard]] inline std::expected<std::uint64_t, minimizer_generation_encoding_error>
emit_grouped_soa_minimizer_generation(p_producer& producer,
                                      const grouped_soa_generation_options& options,
                                      const grouped_soa_minimizer_profile& profile,
                                      grouped_soa_generation_plan& plan,
                                      const minimizer_generation_row_sink_ref& sink) {
    if (!is_grouped_soa_payload_profile(profile) || plan.profile != profile) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    if (!producer_identity_matches(producer, profile.minimizer_scheme, plan.producer_backend)) {
        return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
    }
    return grouped_soa_emit_pass::run(producer, options, profile, plan, sink);
}
/// Build one authenticated fixed metadata row from exact encoded bytes.
[[nodiscard]] inline encoded_minimizer_fixed_row make_fixed_row(const index_chunk_address& address,
                                                                std::span<const std::byte> bytes) {
    encoded_minimizer_fixed_row row{
        .address = address,
        .byte_count = static_cast<std::uint32_t>(bytes.size()),
    };
    std::copy(bytes.begin(), bytes.end(), row.storage.begin());
    row.row_checksum = crc64_index_chunk_row(address.generation_id,
                                             address.position_segment,
                                             address.sub_key,
                                             address.chunk_no,
                                             row.byte_count,
                                             row.bytes());
    return row;
}

} // namespace minimizer_generation_stream_detail

/// Deterministically plan fixed geometry without retaining biological events.
template <grouped_soa_event_producer p_producer>
[[nodiscard]] inline std::expected<grouped_soa_generation_plan, minimizer_generation_encoding_error>
plan_minimizer_generation(p_producer& producer,
                          const grouped_soa_generation_options& options,
                          const grouped_soa_generation_limits& limits = {}) {
    const minimizer_scheme_identity producer_scheme = producer.scheme_identity();
    const minimizer_producer_identity producer_backend = producer.producer_backend_identity();
    const auto profile = derive_grouped_soa_minimizer_profile(
        producer_scheme,
        options.alphabet_code,
        options.kmer_size,
        options.target_length == 0               ? 1u
        : options.position_segment_span_bp == 0u ? k_minimizer_preferred_segment_span_bp
                                                 : options.position_segment_span_bp,
        options.max_occ_cap);
    if (options.layout != minimizer_generation_layout::grouped_soa_segment_local || !profile ||
        !is_supported_minimizer_producer_backend_identity(producer_backend) ||
        minimizer_scheme_identity_for_producer_backend(producer_backend) != producer_scheme ||
        producer_scheme.id != options.minimizer_scheme_id ||
        producer_scheme.revision != options.minimizer_scheme_revision) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    if (options.target_length != 0 &&
        (limits.max_segment_control_count == 0 || limits.max_segment_position_count == 0 ||
         limits.max_hash_position_count == 0)) {
        return std::unexpected(minimizer_generation_encoding_error::element_count);
    }
    auto plan = minimizer_generation_stream_detail::plan_grouped_soa_minimizer_generation(
        producer, options, *profile, producer_backend, limits);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    if (producer.scheme_identity() != producer_scheme ||
        producer.producer_backend_identity() != producer_backend) {
        return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
    }
    plan->producer_backend = producer_backend;
    return plan;
}

/// Stream one immutable qualified generation into a callback-scoped row sink.
template <grouped_soa_event_producer p_producer, minimizer_generation_row_sink p_sink>
[[nodiscard]] inline std::expected<encoded_grouped_soa_generation,
                                   minimizer_generation_encoding_error>
encode_minimizer_generation(p_producer& producer,
                            p_sink& sink,
                            const grouped_soa_generation_options& options,
                            const grouped_soa_generation_limits& limits = {}) {
    if (options.generation_id == 0 || !sql_bigint_can_represent(options.generation_id)) {
        return std::unexpected(minimizer_generation_encoding_error::generation_id);
    }
    const minimizer_scheme_identity requested_scheme{
        .id = options.minimizer_scheme_id,
        .revision = options.minimizer_scheme_revision,
    };
    const auto profile = derive_grouped_soa_minimizer_profile(
        requested_scheme,
        options.alphabet_code,
        options.kmer_size,
        options.target_length == 0               ? 1u
        : options.position_segment_span_bp == 0u ? k_minimizer_preferred_segment_span_bp
                                                 : options.position_segment_span_bp,
        options.max_occ_cap);
    if (options.layout != minimizer_generation_layout::grouped_soa_segment_local || !profile ||
        options.storage_unit_byte_width == 0 || options.kmer_size == 0 ||
        options.window_size == 0 || options.target_length > (std::uint64_t{1} << 63u) ||
        !is_supported_minimizer_scheme_identity(options.minimizer_scheme_id,
                                                options.minimizer_scheme_revision)) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    static_assert(k_grouped_soa_extent_payload_bytes %
                      grouped_soa_bucket_directory_entry::encoded_bytes ==
                  0);
    if constexpr (requires { producer.bind_generation_layout(options.layout); }) {
        /* Stats-bearing producers freeze their successful replay count
         * before the planning scan.  Generic/vector producers need no hook. */
        producer.bind_generation_layout(options.layout);
    }
    auto plan = plan_minimizer_generation(producer, options, limits);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    if (!is_supported_minimizer_producer_backend_identity(plan->producer_backend) ||
        producer.producer_backend_identity() != plan->producer_backend) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    const auto storage_plan =
        minimizer_generation_stream_detail::make_minimizer_generation_storage_plan(options, *plan);
    if (!storage_plan) {
        return std::unexpected(storage_plan.error());
    }
    if constexpr (planned_grouped_soa_generation_sink<p_sink>) {
        if (auto begun = sink.begin_planned_generation(*storage_plan); !begun) {
            return std::unexpected(begun.error());
        }
    }
    minimizer_generation_row_sink_ref sink_ref{
        .context = &sink,
        .write_fn =
            [](void* context, const minimizer_generation_row_view& row) {
                return static_cast<p_sink*>(context)->write(row);
            },
    };
    if constexpr (grouped_soa_minimizer_payload_sink<p_sink>) {
        sink_ref.write_grouped_soa_payload_fn =
            [](void* context, const unchecked_minimizer_generation_row_view& row) {
                return static_cast<p_sink*>(context)->write_grouped_soa_payload(row);
            };
    }
    auto payload_checksum =
        minimizer_generation_stream_detail::emit_grouped_soa_minimizer_generation(
            producer, options, plan->profile, *plan, sink_ref);
    if (!payload_checksum) {
        return std::unexpected(payload_checksum.error());
    }
    if (producer.producer_backend_identity() != plan->producer_backend) {
        return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
    }

    auto directory_bytes = encode_minimizer_bucket_directory(plan->entries, plan->profile);
    if (!directory_bytes) {
        return std::unexpected(directory_bytes.error());
    }
    if (directory_bytes->size() != storage_plan->directory_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::directory_size);
    }
    auto directory = grouped_soa_directory_view::decode(*directory_bytes, plan->profile);
    if (!directory) {
        return std::unexpected(directory.error());
    }
    const std::uint32_t extent_payload_bytes = minimizer_extent_payload_bytes(options.layout);
    if (extent_payload_bytes == 0) {
        return std::unexpected(minimizer_generation_encoding_error::unsupported_storage_layout);
    }
    const std::uint64_t directory_chunk_count =
        directory_bytes->size() / extent_payload_bytes +
        (directory_bytes->size() % extent_payload_bytes != 0 ? 1u : 0u);
    if (!sql_integer_can_represent(directory_chunk_count)) {
        return std::unexpected(minimizer_generation_encoding_error::chunk_range);
    }

    std::uint64_t control_count = 0;
    std::uint64_t position_count = 0;
    std::uint64_t payload_bytes = 0;
    for (const grouped_soa_bucket_directory_entry& entry : plan->entries) {
        control_count = bioseq::common::saturating_add(control_count, entry.control_element_count);
        position_count =
            bioseq::common::saturating_add(position_count, entry.position_element_count);
        payload_bytes = bioseq::common::saturating_add(
            payload_bytes,
            bioseq::common::saturating_add(entry.control_encoded_bytes,
                                           entry.position_encoded_bytes));
    }
    if (plan->entries.size() > (std::numeric_limits<std::uint32_t>::max)() / 2u ||
        control_count == (std::numeric_limits<std::uint64_t>::max)() ||
        position_count == (std::numeric_limits<std::uint64_t>::max)() ||
        payload_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        payload_bytes != storage_plan->payload_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    minimizer_generation_manifest manifest{
        .layout = options.layout,
        .generation_id = options.generation_id,
        .source_identity = options.source_identity,
        .source_content_checksum = options.source_content_checksum,
        .target_length = options.target_length,
        .control_count = control_count,
        .position_count = position_count,
        .payload_bytes = payload_bytes,
        .payload_checksum = *payload_checksum,
        .directory_bytes = directory_bytes->size(),
        .directory_checksum = crc64_ecma182(*directory_bytes),
        .position_segment_count = plan->position_segment_count,
        .extent_count = static_cast<std::uint32_t>(plan->entries.size() * 2u),
        .directory_chunk_count = static_cast<std::uint32_t>(directory_chunk_count),
        .alphabet_code = options.alphabet_code,
        .storage_unit_byte_width = options.storage_unit_byte_width,
        .kmer_size = options.kmer_size,
        .window_size = options.window_size,
        .gpu_abi_revision = plan->profile.gpu_abi_revision,
        .position_segment_span_bp = plan->position_segment_span_bp,
        .segment_geometry_revision = k_minimizer_segment_geometry_revision,
        .minimizer_scheme_id = options.minimizer_scheme_id,
        .minimizer_scheme_revision = options.minimizer_scheme_revision,
        .producer_backend_id = plan->producer_backend.id,
        .producer_identity_revision = plan->producer_backend.revision,
        .hash_width = plan->profile.hash_width,
        .position_width = plan->profile.position_width,
        .count_width = plan->profile.count_width,
        .offset_policy = plan->profile.offset_policy,
        .control_layout = plan->profile.control_layout,
        .count_semantics = plan->profile.count_semantics,
        .position_semantics = plan->profile.position_semantics,
        .max_hash = plan->profile.max_hash,
        .max_occ_cap = plan->profile.max_occ_cap,
    };
    const auto manifest_bytes = encode_minimizer_generation_manifest_record(manifest);
    if (!manifest_bytes) {
        return std::unexpected(manifest_bytes.error());
    }
    if (manifest_bytes->size() != storage_plan->manifest_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::encoded_size);
    }
    minimizer_generation_head head{
        .layout = options.layout,
        .generation_id = options.generation_id,
        .source_identity = options.source_identity,
        .storage_contract_checksum =
            compute_minimizer_generation_storage_contract_checksum(manifest),
        .manifest_checksum = crc64_ecma182(std::span<const std::byte>{*manifest_bytes}),
        .source_content_checksum = options.source_content_checksum,
    };
    const auto head_bytes = encode_minimizer_generation_head(head);
    if (!head_bytes) {
        return std::unexpected(head_bytes.error());
    }

    const encoded_minimizer_fixed_row manifest_row =
        minimizer_generation_stream_detail::make_fixed_row(
            index_chunk_address{
                .generation_id = options.generation_id,
                .position_segment = -1,
                .sub_key = k_directory_sub_key,
                .chunk_no = 0,
            },
            *manifest_bytes);
    const encoded_minimizer_fixed_row unpublished_head_row =
        minimizer_generation_stream_detail::make_fixed_row(
            index_chunk_address{
                .generation_id = 0,
                .position_segment = -1,
                .sub_key = k_generation_head_sub_key,
                .chunk_no = 0,
            },
            *head_bytes);
    if (auto metadata = validate_minimizer_generation_metadata(head, manifest, *directory);
        !metadata) {
        return std::unexpected(metadata.error());
    }

    // Publication boundary: payload was written by emit_pass above; only the
    // directory and READY manifest are appended here. The active head is
    // returned to the caller and must be published only after a storage
    // reread validates this complete generation.
    for (std::size_t offset = 0, chunk = 1; offset < directory_bytes->size();
         offset += extent_payload_bytes, ++chunk) {
        const std::size_t count =
            std::min<std::size_t>(extent_payload_bytes, directory_bytes->size() - offset);
        const auto bytes = std::span<const std::byte>{*directory_bytes}.subspan(offset, count);
        const index_chunk_address address{
            .generation_id = options.generation_id,
            .position_segment = -1,
            .sub_key = k_directory_sub_key,
            .chunk_no = static_cast<std::uint32_t>(chunk),
        };
        const std::uint64_t checksum =
            crc64_index_chunk_row(address.generation_id,
                                  address.position_segment,
                                  address.sub_key,
                                  address.chunk_no,
                                  static_cast<std::uint32_t>(bytes.size()),
                                  bytes);
        if (auto written = sink.write(minimizer_generation_row_view{
                .address = address, .bytes = bytes, .row_checksum = checksum});
            !written) {
            return std::unexpected(written.error());
        }
    }
    if (auto written =
            sink.write(minimizer_generation_row_view{.address = manifest_row.address,
                                                     .bytes = manifest_row.bytes(),
                                                     .row_checksum = manifest_row.row_checksum});
        !written) {
        return std::unexpected(written.error());
    }
    return encoded_grouped_soa_generation{
        .head = head,
        .manifest = manifest,
        .unpublished_head_row = unpublished_head_row,
        .manifest_row = manifest_row,
        .directory = std::move(*directory),
    };
}

/// Streaming complete-generation validator for ordered qualified rows.
class minimizer_generation_row_validator {
  private:
    struct validated_tag {};

    encoded_grouped_soa_generation generation_{};
    std::size_t entry_index_ = 0;
    std::uint32_t payload_chunk_ = 0;
    std::uint32_t directory_chunk_ = 1;
    crc64_ecma182_accumulator extent_crc_{};
    crc64_ecma182_accumulator payload_row_checksums_{};
    bool position_payload_stage_ = false;
    bool manifest_seen_ = false;
    bool ready_validated_ = false;

    explicit minimizer_generation_row_validator(validated_tag,
                                                const encoded_grouped_soa_generation& generation)
        : generation_(generation) {}

    [[nodiscard]] static std::expected<void, minimizer_generation_encoding_error>
    validate_fixed_row(const encoded_minimizer_fixed_row& row,
                       const index_chunk_address& expected_address,
                       std::span<const std::byte> expected_bytes) noexcept {
        if (row.address != expected_address || row.byte_count != expected_bytes.size() ||
            row.byte_count > row.storage.size() ||
            !std::ranges::equal(row.bytes(), expected_bytes)) {
            return std::unexpected(minimizer_generation_encoding_error::row_order);
        }
        const std::uint64_t expected_checksum = crc64_index_chunk_row(row.address.generation_id,
                                                                      row.address.position_segment,
                                                                      row.address.sub_key,
                                                                      row.address.chunk_no,
                                                                      row.byte_count,
                                                                      row.bytes());
        if (row.row_checksum != expected_checksum) {
            return std::unexpected(minimizer_generation_encoding_error::checksum);
        }
        return {};
    }

  public:
    /// Validate every metadata equation and both fixed rows before any caller
    /// can enter row/subspan arithmetic.
    [[nodiscard]] static std::expected<minimizer_generation_row_validator,
                                       minimizer_generation_encoding_error>
    create(const encoded_grouped_soa_generation& generation) {
        if (auto metadata = validate_minimizer_generation_metadata(
                generation.head, generation.manifest, generation.directory);
            !metadata) {
            return std::unexpected(metadata.error());
        }
        const auto manifest_bytes =
            encode_minimizer_generation_manifest_record(generation.manifest);
        const auto head_bytes = encode_minimizer_generation_head(generation.head);
        if (!manifest_bytes || !head_bytes) {
            return std::unexpected(!manifest_bytes ? manifest_bytes.error() : head_bytes.error());
        }
        if (auto fixed = validate_fixed_row(generation.manifest_row,
                                            index_chunk_address{
                                                .generation_id = generation.manifest.generation_id,
                                                .position_segment = -1,
                                                .sub_key = k_directory_sub_key,
                                                .chunk_no = 0,
                                            },
                                            *manifest_bytes);
            !fixed) {
            return std::unexpected(fixed.error());
        }
        if (auto fixed = validate_fixed_row(generation.unpublished_head_row,
                                            index_chunk_address{
                                                .generation_id = 0,
                                                .position_segment = -1,
                                                .sub_key = k_generation_head_sub_key,
                                                .chunk_no = 0,
                                            },
                                            *head_bytes);
            !fixed) {
            return std::unexpected(fixed.error());
        }
        for (const grouped_soa_bucket_directory_entry& entry : generation.directory.entries()) {
            if (entry.control_chunk_count >
                (std::numeric_limits<std::uint32_t>::max)() - entry.position_chunk_count) {
                return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
            }
        }
        return minimizer_generation_row_validator{validated_tag{}, generation};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    accept(const minimizer_generation_row_view& row) noexcept {
        if (ready_validated_ || manifest_seen_) {
            return std::unexpected(minimizer_generation_encoding_error::row_count);
        }
        const std::uint32_t extent_payload_bytes =
            minimizer_extent_payload_bytes(generation_.manifest.layout);
        if (extent_payload_bytes == 0) {
            return std::unexpected(minimizer_generation_encoding_error::unsupported_storage_layout);
        }
        if (entry_index_ < generation_.directory.entries().size()) {
            const grouped_soa_bucket_directory_entry& entry =
                generation_.directory.entries()[entry_index_];
            if (entry.control_chunk_count >
                (std::numeric_limits<std::uint32_t>::max)() - entry.position_chunk_count) {
                return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
            }
            const bool grouped_soa = generation_.manifest.layout ==
                                     minimizer_generation_layout::grouped_soa_segment_local;
            const bool control =
                grouped_soa ? !position_payload_stage_ : payload_chunk_ < entry.control_chunk_count;
            const std::uint32_t extent_chunks =
                control ? entry.control_chunk_count : entry.position_chunk_count;
            const std::uint32_t total_chunks =
                grouped_soa ? extent_chunks
                            : entry.control_chunk_count + entry.position_chunk_count;
            if (payload_chunk_ >= total_chunks) {
                return std::unexpected(minimizer_generation_encoding_error::row_count);
            }
            const auto position_sub_key = derive_minimizer_position_extent_sub_key(entry.sub_key);
            if (!position_sub_key) {
                return std::unexpected(position_sub_key.error());
            }
            const std::uint32_t extent_chunk = control ? payload_chunk_
                                               : grouped_soa
                                                   ? payload_chunk_
                                                   : payload_chunk_ - entry.control_chunk_count;
            const std::uint64_t extent_bytes =
                control ? entry.control_encoded_bytes : entry.position_encoded_bytes;
            if (extent_chunk > (std::numeric_limits<std::uint64_t>::max)() / extent_payload_bytes) {
                return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
            }
            const std::uint64_t offset =
                static_cast<std::uint64_t>(extent_chunk) * extent_payload_bytes;
            if (offset >= extent_bytes) {
                return std::unexpected(minimizer_generation_encoding_error::byte_count);
            }
            const std::uint64_t expected_bytes =
                std::min<std::uint64_t>(extent_payload_bytes, extent_bytes - offset);
            if (row.address.generation_id != generation_.manifest.generation_id ||
                row.address.position_segment != entry.position_segment ||
                row.address.sub_key != (control ? entry.sub_key : *position_sub_key) ||
                row.address.chunk_no != extent_chunk || row.bytes.size() != expected_bytes) {
                return std::unexpected(minimizer_generation_encoding_error::row_order);
            }
            const std::uint64_t expected_checksum =
                crc64_index_chunk_row(row.address.generation_id,
                                      row.address.position_segment,
                                      row.address.sub_key,
                                      row.address.chunk_no,
                                      static_cast<std::uint32_t>(row.bytes.size()),
                                      row.bytes);
            if (row.row_checksum != expected_checksum) {
                return std::unexpected(minimizer_generation_encoding_error::checksum);
            }
            extent_crc_.update(row.bytes);
            payload_row_checksums_.update_little_endian(row.row_checksum);
            ++payload_chunk_;
            const bool extent_complete =
                grouped_soa
                    ? payload_chunk_ == extent_chunks
                    : payload_chunk_ == (control ? entry.control_chunk_count : total_chunks);
            if (extent_complete && control) {
                if (extent_crc_.checksum() != entry.control_checksum) {
                    return std::unexpected(minimizer_generation_encoding_error::checksum);
                }
                extent_crc_ = {};
            }
            if (extent_complete && !control) {
                if (extent_crc_.checksum() != entry.position_checksum) {
                    return std::unexpected(minimizer_generation_encoding_error::checksum);
                }
                extent_crc_ = {};
                payload_chunk_ = 0;
                ++entry_index_;
            }
            if (grouped_soa && extent_complete && control) {
                payload_chunk_ = 0;
                ++entry_index_;
                if (entry_index_ == generation_.directory.entries().size()) {
                    entry_index_ = 0;
                    position_payload_stage_ = true;
                }
            }
            return {};
        }
        if (directory_chunk_ <= generation_.manifest.directory_chunk_count) {
            const std::uint64_t chunk_index = static_cast<std::uint64_t>(directory_chunk_ - 1u);
            if (chunk_index > (std::numeric_limits<std::uint64_t>::max)() / extent_payload_bytes) {
                return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
            }
            const std::uint64_t offset64 = chunk_index * extent_payload_bytes;
            if (offset64 >= generation_.directory.encoded_bytes().size() ||
                offset64 > (std::numeric_limits<std::size_t>::max)()) {
                return std::unexpected(minimizer_generation_encoding_error::directory_size);
            }
            const std::size_t offset = static_cast<std::size_t>(offset64);
            const std::size_t count = std::min<std::size_t>(
                extent_payload_bytes, generation_.directory.encoded_bytes().size() - offset);
            const auto expected_bytes =
                generation_.directory.encoded_bytes().subspan(offset, count);
            if (row.address !=
                    index_chunk_address{
                        .generation_id = generation_.manifest.generation_id,
                        .position_segment = -1,
                        .sub_key = k_directory_sub_key,
                        .chunk_no = directory_chunk_,
                    } ||
                !std::ranges::equal(row.bytes, expected_bytes)) {
                return std::unexpected(minimizer_generation_encoding_error::row_order);
            }
            const std::uint64_t checksum =
                crc64_index_chunk_row(row.address.generation_id,
                                      row.address.position_segment,
                                      row.address.sub_key,
                                      row.address.chunk_no,
                                      static_cast<std::uint32_t>(row.bytes.size()),
                                      row.bytes);
            if (row.row_checksum != checksum) {
                return std::unexpected(minimizer_generation_encoding_error::checksum);
            }
            ++directory_chunk_;
            return {};
        }
        if (!manifest_seen_) {
            if (row.address != generation_.manifest_row.address ||
                row.row_checksum != generation_.manifest_row.row_checksum ||
                !std::ranges::equal(row.bytes, generation_.manifest_row.bytes())) {
                return std::unexpected(minimizer_generation_encoding_error::row_order);
            }
            manifest_seen_ = true;
            return {};
        }
        return std::unexpected(minimizer_generation_encoding_error::row_count);
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error> finish_ready() noexcept {
        if (ready_validated_) {
            return {};
        }
        if (entry_index_ != generation_.directory.entries().size() || payload_chunk_ != 0 ||
            (generation_.manifest.layout ==
                 minimizer_generation_layout::grouped_soa_segment_local &&
             !generation_.directory.entries().empty() && !position_payload_stage_) ||
            directory_chunk_ != generation_.manifest.directory_chunk_count + 1u ||
            !manifest_seen_) {
            return std::unexpected(minimizer_generation_encoding_error::row_count);
        }
        if (payload_row_checksums_.checksum() != generation_.manifest.payload_checksum) {
            return std::unexpected(minimizer_generation_encoding_error::checksum);
        }
        ready_validated_ = true;
        return {};
    }

    /// Validate the separately published active-head row only after READY was
    /// completely reread and accepted.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    validate_publication_head(const minimizer_generation_row_view& row) const noexcept {
        if (!ready_validated_) {
            return std::unexpected(minimizer_generation_encoding_error::row_count);
        }
        const encoded_minimizer_fixed_row& expected = generation_.unpublished_head_row;
        if (row.address != expected.address || row.row_checksum != expected.row_checksum ||
            !std::ranges::equal(row.bytes, expected.bytes())) {
            return std::unexpected(minimizer_generation_encoding_error::row_order);
        }
        return {};
    }
};

} // namespace bioseq::indexing
