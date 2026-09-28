/**
 * @file minimizer_hashmap/generation/publication.cppm
 * @brief Encode grouped-SoA rows and prepare metadata for publication.
 */
export module bioseq.indexing.minimizer_hashmap:generation_publication;

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
import :generation_stream;

export namespace bioseq::indexing {

/// Check the registered algorithm against the producer and requested storage  profile.
[[nodiscard]] constexpr bool grouped_soa_generation_algorithm_identity_matches(
    const grouped_soa_generation_options& options,
    const grouped_soa_minimizer_profile& profile,
    minimizer_producer_identity producer) noexcept {
    const auto& identity = options.algorithm_identity;
    return is_valid_minimizer_generation_algorithm_identity(identity, producer) &&
           identity.alphabet_id == options.alphabet_code &&
           identity.packed_storage_bits == options.storage_unit_byte_width * 8u &&
           identity.runtime_kmer_size == options.kmer_size &&
           identity.runtime_window_size == options.window_size &&
           identity.hash_width == profile.hash_width;
}

/// Plan directory entries and byte ranges by scanning the producer's events.
/// Keep counts and checksums to check later scans. Retry with smaller segments
/// when a count exceeds its limit. The producer's scheme and backend must stay
/// unchanged throughout planning.
/// @tparam p_producer Replayable source of sorted minimizer events.
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
    if constexpr (requires { producer.algorithm_identity(); }) {
        if (producer.algorithm_identity() != options.algorithm_identity) {
            return std::unexpected(
                minimizer_generation_encoding_error::storage_contract_mismatch);
        }
    }
    if (options.layout != minimizer_generation_layout::grouped_soa_segment_local || !profile ||
        !is_supported_minimizer_producer_backend_identity(producer_backend) ||
        !grouped_soa_generation_algorithm_identity_matches(options, *profile, producer_backend) ||
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
    if constexpr (requires { producer.algorithm_identity(); }) {
        if (producer.algorithm_identity() != options.algorithm_identity) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
    }
    plan->producer_backend = producer_backend;
    return plan;
}

/// Build manifest fields from the directory plan and supplied checksums.
/// Direct sinks use provisional checksums before writing payload. The encoder
/// calls the same function with final checksums, keeping byte counts and segment
/// layout consistent between  both stages.
[[nodiscard]] inline std::expected<minimizer_generation_manifest,
                                   minimizer_generation_encoding_error>
make_grouped_soa_generation_manifest(
    const grouped_soa_generation_options& options,
    const grouped_soa_generation_plan& plan,
    std::uint64_t directory_bytes,
    std::uint64_t directory_checksum,
    std::uint64_t payload_checksum) noexcept {
    const std::uint32_t extent_payload_bytes = minimizer_extent_payload_bytes(options.layout);
    const std::uint64_t directory_entry_bytes =
        minimizer_directory_entry_encoded_bytes(plan.profile);
    if (extent_payload_bytes == 0u) {
        return std::unexpected(
            minimizer_generation_encoding_error::unsupported_storage_layout);
    }
    if (directory_entry_bytes == 0u ||
        options.layout != minimizer_generation_layout::grouped_soa_segment_local ||
        plan.profile.layout != options.layout) {
        return std::unexpected(minimizer_generation_encoding_error::directory_size);
    }
    if (plan.entries.size() > (std::numeric_limits<std::uint32_t>::max)() / 2u ||
        plan.entries.size() > (std::numeric_limits<std::uint64_t>::max)() /
                                  directory_entry_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    if (directory_bytes != plan.entries.size() * directory_entry_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::directory_size);
    }
    const std::uint64_t directory_chunk_count =
        directory_bytes / extent_payload_bytes +
        (directory_bytes % extent_payload_bytes != 0u ? 1u : 0u);
    if (!sql_integer_can_represent(directory_chunk_count)) {
        return std::unexpected(minimizer_generation_encoding_error::chunk_range);
    }

    std::uint64_t control_count = 0u;
    std::uint64_t position_count = 0u;
    std::uint64_t payload_bytes = 0u;
    for (const grouped_soa_bucket_directory_entry& entry : plan.entries) {
        control_count = bioseq::common::saturating_add(
            control_count, entry.control_element_count);
        position_count = bioseq::common::saturating_add(
            position_count, entry.position_element_count);
        payload_bytes = bioseq::common::saturating_add(
            payload_bytes,
            bioseq::common::saturating_add(entry.control_encoded_bytes,
                                           entry.position_encoded_bytes));
    }
    if (control_count == (std::numeric_limits<std::uint64_t>::max)() ||
        position_count == (std::numeric_limits<std::uint64_t>::max)() ||
        payload_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    return minimizer_generation_manifest{
        .layout = options.layout,
        .generation_id = options.generation_id,
        .source_identity = options.source_identity,
        .source_content_checksum = options.source_content_checksum,
        .target_length = options.target_length,
        .control_count = control_count,
        .position_count = position_count,
        .payload_bytes = payload_bytes,
        .payload_checksum = payload_checksum,
        .directory_bytes = directory_bytes,
        .directory_checksum = directory_checksum,
        .position_segment_count = plan.position_segment_count,
        .extent_count = static_cast<std::uint32_t>(plan.entries.size() * 2u),
        .directory_chunk_count = static_cast<std::uint32_t>(directory_chunk_count),
        .alphabet_code = options.alphabet_code,
        .storage_unit_byte_width = options.storage_unit_byte_width,
        .kmer_size = options.kmer_size,
        .window_size = options.window_size,
        .gpu_abi_revision = plan.profile.gpu_abi_revision,
        .position_segment_span_bp = plan.position_segment_span_bp,
        .segment_geometry_revision = k_minimizer_segment_geometry_revision,
        .minimizer_scheme_id = options.minimizer_scheme_id,
        .minimizer_scheme_revision = options.minimizer_scheme_revision,
        .producer_backend_id = plan.producer_backend.id,
        .producer_identity_revision = plan.producer_backend.revision,
        .algorithm_identity = options.algorithm_identity,
        .hash_width = plan.profile.hash_width,
        .position_width = plan.profile.position_width,
        .count_width = plan.profile.count_width,
        .offset_policy = plan.profile.offset_policy,
        .control_layout = plan.profile.control_layout,
        .count_semantics = plan.profile.count_semantics,
        .position_semantics = plan.profile.position_semantics,
        .max_hash = plan.profile.max_hash,
        .max_occ_cap = plan.profile.max_occ_cap,
    };
}

/// Write payload rows, directory rows, and the READY manifest to the sink.
/// Return the active-head row for the caller to publish after validating a
/// complete storage reread. A failure can leave, incomplete rows in the sink.
/// These rows must stay inactive.
///
/// @tparam p_producer Replayable source of sorted minimiser events.
/// @tparam p_sink Borrowed sink receiving rows synchronously
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
        !grouped_soa_generation_algorithm_identity_matches(
            options, *profile, producer.producer_backend_identity()) ||
        !is_supported_minimizer_scheme_identity(options.minimizer_scheme_id,
                                                options.minimizer_scheme_revision)) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    static_assert(k_grouped_soa_extent_payload_bytes %
                      grouped_soa_bucket_directory_entry::encoded_bytes ==
                  0);
    if constexpr (requires { producer.bind_generation_layout(options.layout); }) {
        /* Record the layout before planning so execution stats can check
         * how many scans a successful encoding requires. */
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
    if constexpr (direct_grouped_soa_generation_sink<p_sink>) {
        if (auto begun = sink.begin_direct_grouped_soa_generation(
                direct_grouped_soa_generation_plan_view{
                    .storage_plan = *storage_plan,
                    .options = options,
                    .generation_plan = *plan,
                });
            !begun) {
            return std::unexpected(begun.error());
        }
    } else if constexpr (planned_grouped_soa_generation_sink<p_sink>) {
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
    if constexpr (requires { producer.algorithm_identity(); }) {
        if (producer.algorithm_identity() != options.algorithm_identity) {
            return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
        }
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
    auto completed_manifest = make_grouped_soa_generation_manifest(
        options,
        *plan,
        directory_bytes->size(),
        crc64_ecma182(*directory_bytes),
        *payload_checksum);
    if (!completed_manifest) {
        return std::unexpected(completed_manifest.error());
    }
    minimizer_generation_manifest manifest = std::move(*completed_manifest);
    if (manifest.payload_bytes != storage_plan->payload_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
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

    // Payload rows are  complete. Append the directory and READY
    // manifest, then return the active head. The caller publishes that
    // head after a complete storage reread validates the generation.
    const std::uint32_t extent_payload_bytes =
        minimizer_extent_payload_bytes(options.layout);
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
    encoded_grouped_soa_generation encoded{
        .head = head,
        .manifest = manifest,
        .unpublished_head_row = unpublished_head_row,
        .manifest_row = manifest_row,
        .directory = std::move(*directory),
    };
    if constexpr (direct_grouped_soa_generation_sink<p_sink>) {
        if (auto sealed = sink.seal_direct_grouped_soa_generation(encoded); !sealed) {
            return std::unexpected(sealed.error());
        }
    }
    return encoded;
}

} // namespace bioseq::indexing
