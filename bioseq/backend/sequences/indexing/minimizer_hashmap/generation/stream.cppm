/**
 * @file minimizer_hashmap/generation/stream.cppm
 * @brief Plans and streams grouped-SoA control and position payload rows.
 */
export module bioseq.indexing.minimizer_hashmap:generation_stream;

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
import :generation_contracts;

export namespace bioseq::indexing {

namespace minimizer_generation_stream_detail {

/// Reserved bytes before the first planned payload section.
inline constexpr std::uint64_t k_planned_generation_image_header_bytes = 4096u;
/// Alignment shared by planned directory and manifest sections.
inline constexpr std::uint64_t k_planned_generation_section_alignment = 8u;

/// Add image sizes or offsets, returning arithmetic_overflow if they cannot fit.
[[nodiscard]] constexpr std::expected<std::uint64_t, minimizer_generation_encoding_error>
planned_storage_checked_add(std::uint64_t left, std::uint64_t right) noexcept {
    if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    return left + right;
}

/// Round an image offset up to the section alignment, checking overflow.
[[nodiscard]] constexpr std::expected<std::uint64_t, minimizer_generation_encoding_error>
planned_storage_align_up(std::uint64_t value) noexcept {
    const std::uint64_t remainder = value % k_planned_generation_section_alignment;
    return planned_storage_checked_add(
        value, remainder == 0u ? 0u : k_planned_generation_section_alignment - remainder);
}

/// Calculate payload, directory, and manifest sizes and offsets before writing rows.
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

/// Add the event's hash and encoded position, as little-endian 64-bit values,
/// to the checksum used to  compare producer scans.
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

/// Multiply element count by its stored width, rejecting zero width or overflow.
[[nodiscard]] constexpr std::expected<std::uint64_t, minimizer_generation_encoding_error>
checked_payload_byte_count(std::uint64_t element_count, std::uint64_t encoded_byte_width) noexcept {
    if (encoded_byte_width == 0 ||
        element_count > (std::numeric_limits<std::uint64_t>::max)() / encoded_byte_width) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    return element_count * encoded_byte_width;
}

/// Count payload rows, including a final partial row when needed.
[[nodiscard]] constexpr std::uint64_t
grouped_soa_payload_chunk_count(std::uint64_t encoded_bytes) noexcept {
    constexpr std::uint32_t extent_payload_bytes = k_grouped_soa_extent_payload_bytes;
    static_assert(extent_payload_bytes != 0);
    return encoded_bytes / extent_payload_bytes +
           (encoded_bytes % extent_payload_bytes != 0 ? 1u : 0u);
}

/// Plan hash groups and position ranges without storing their events.
/// Keep every hash group in one entry. Start a new entry when adding a group
/// would exceed the target size or require wider prefix offsets. A large group
/// can exceed the target size on its own.
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

    /// Add the current partition's sizes, hash bounds, and addresses to the directorw.
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

    /// Calculate control and position bytes for  a proposed partition.
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

    /// Add the current hash group Start a new partition if adding it would
    /// exceed the size target or require wider prefix offsets.
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

    /// Check event order and bounds, then update group and segment counts.
    /// If a count limit is exceeded, request another scan with, smaller segments.
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

    /// Flush the final hash group and partition.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error> finish() {
        if (auto group = finish_group(); !group)
            return group;
        return flush_partition();
    }
};

/// Forward a producer callback to the planning pass.
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
receive_grouped_soa_plan_event(void* context, const canonical_minimizer_event& event) {
    return static_cast<grouped_soa_plan_pass*>(context)->accept(event);
}

/// Check that the producer still reports the planned scheme and backend.
/// @tparam p_producer Replayable source of sorted minimizer events.
template <grouped_soa_event_producer p_producer>
[[nodiscard]] inline bool producer_identity_matches(const p_producer& producer,
                                                    minimizer_scheme_identity scheme,
                                                    minimizer_producer_identity backend) noexcept {
    return producer.scheme_identity() == scheme && producer.producer_backend_identity() == backend;
}

/// Stream an extent through one reusable 8,064-byte row buffer.
/// Control and position passes share  this writer. After each sink write,
/// add the returned row CRC to the generation checksum in write order.
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

    /// Start an extent with the row address and planned byte and chunk counts.
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

    /// Send the buffered row to the sink and include its CRC in the payload checksum.
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

    /// Append bytes up to the planned extent length, flushing full rows as needed.
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

    /// Write the final partial row, check the planned totals, and return the extent CRC.
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

/// Build and write one control extenv at a time while replaying events.
/// Store hashes, prefix offsets, and capped counts. Position values are counted
/// for  this pass. Zero-filled storage also gives alignment padding a checked value
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

    /// Check the profile, segment span, plan pointers, and sink before control replay.
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

    /// Check the next planned entry and allocate its zero-filled control image.
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

    /// Store this group's hash, prefix endpoints, and count capped at max_occ_cap + 1.
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

    /// Finish and write the control extent, then save its CRC in the directory entry.
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

    /// Check one replayed event  and count it toward the current hash group's control.
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

    /// Write any final, control extent and require every planned entry to be filled.
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

/// Stream segment-local positions through the fixed row buffer.
/// Even a hash with many positions uses the same buffer size while its rows
/// are written one after another.
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

    /// Check the profile, segment span, pgan pointers, and sink before position replay.
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

    /// Check the  next planned entry and start writing its position extent.
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

    /// Count the completed hash group and reset its position count.
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

    /// Finish the position extent, check counts, and save its CRC in the directory entry.
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

    /// Check an event, convert its position to a segment-local value, and append
    /// it with the original strand bit
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

    /// Finish the last position extent and require every planned entry to be  filled.
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

/// Write all control extents, then all position extents, using two producer scans.
struct grouped_soa_emit_pass {
    /// Replay events for controls and positions, checking identity, event count,
    /// and event checksum against the plan after each pass. Return the checksum
    /// of row CRCs in write order. Errors may follow rows already sent to the sink.
    /// @tparam p_producer Replayable source of sorted minimizer events.
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

/// Scan events to plan grouped-SoA entries and their byte ranges.
/// Halve the segment width and retry when count limits are exceeded. Preserve
/// the successful scan's event count and checksum for later  eplay checks.
/// @tparam p_producer Replayable source of sorted minimizer events.
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
            /* Empty generations store a zero segment span. Use one while
             * invoking the, producer, which requires a nonzero scan span. */
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

/// Check the plan and producer identity, then write the control and position rows.
/// @tparam p_producer Replayable source of sorted minimizer events.
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
/// Copy a head or manifest into fixed row storage and compute its address-bound CRC.
/// The encoded bytes must fit encoded_minimizer_fixed_row::storage.
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

} // namespace bioseq::indexing
