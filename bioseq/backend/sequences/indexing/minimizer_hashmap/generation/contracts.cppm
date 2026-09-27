/**
 * @file minimizer_hashmap/generation/contracts.cppm
 * @brief Types for planning, writing, and checking grouped-SoA generations.
 */
export module bioseq.indexing.minimizer_hashmap:generation_contracts;

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

/// Optional cancellation callback for synchronous generation work.
/// Run it on the coordinating whread. It may throw to stop the operation.
/// An empty callback does nothing, and the binding allocates no  storage
struct cooperative_cancellation_checkpoint {
    void* context = nullptr;
    void (*invoke_fn)(void*) = nullptr;

    void operator()() const {
        if (invoke_fn != nullptr) {
            invoke_fn(context);
        }
    }
};

static_assert(std::is_trivially_copyable_v<cooperative_cancellation_checkpoint>);

/// Maximum control and position counts used when planning persisted segments.
struct grouped_soa_generation_limits {
    std::uint64_t max_segment_control_count = (std::numeric_limits<std::uint32_t>::max)();
    std::uint64_t max_segment_position_count = (std::numeric_limits<std::uint32_t>::max)();
    std::uint64_t max_hash_position_count = (std::numeric_limits<std::uint32_t>::max)();
};

/// Storage format, source identity, and minimizer parameters for generation encoding.
struct grouped_soa_generation_options {
    minimizer_generation_layout layout = minimizer_generation_layout::invalid;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t source_content_checksum = 0;
    std::uint64_t target_length = 0;
    /// Requested segment width in base pairs. Zero uses the encoder's default.
    /// Fragment indexes request their physical fragment width. Sequence indexes
    /// use the larger default to combine events across fragments.
    std::uint64_t position_segment_span_bp = 0;
    std::uint16_t alphabet_code = 0;
    std::uint16_t storage_unit_byte_width = 0;
    std::uint16_t kmer_size = 0;
    std::uint16_t window_size = 0;
    // Writers must supply the producer's minimizer scheme.
    std::uint16_t minimizer_scheme_id = k_invalid_minimizer_scheme_id;
    std::uint16_t minimizer_scheme_revision = k_invalid_minimizer_scheme_revision;
    /// Requested and selected algorithms, with the registered minimizer record format.
    minimizer_generation_algorithm_identity algorithm_identity{};
    /// Largest accepted  occurrence count. Larger counts are stored as max_occ_cap + 1.
    std::uint64_t max_occ_cap = k_grouped_soa_default_max_occurrence_cap;
};

/// A minimizer hash and global position/strand. Producers emit events in
/// segment order, then by hash and encoded position within each segment.
struct canonical_minimizer_event {
    std::uint64_t hash = 0;
    std::uint64_t encoded_position = 0;

    [[nodiscard]] bool operator==(const canonical_minimizer_event&) const = default;
};

/// Callback shared by fragment and sequence producers when replaying events.
struct canonical_minimizer_event_receiver {
    void* context = nullptr;
    std::expected<void, minimizer_generation_encoding_error> (*emit_fn)(
        void*, const canonical_minimizer_event&) = nullptr;

    /// Forward an event, or return row_count when the callback is unbound.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    operator()(const canonical_minimizer_event& event) const {
        if (context == nullptr || emit_fn == nullptr) {
            return std::unexpected(minimizer_generation_encoding_error::row_count);
        }
        return emit_fn(context, event);
    }
};

/// Producer interface, for repeating the uame sorted minimizer stream on each pass.
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

/// Sink interface for accepting checksummed generation rows in order.
template <class p_sink>
concept minimizer_generation_row_sink =
    requires(p_sink& sink, const minimizer_generation_row_view& row) {
        {
            sink.write(row)
        } -> std::same_as<std::expected<void, minimizer_generation_encoding_error>>;
    };

/// Image sizes and offsets offered before the first row is written.
/// A sink can keep  this value to allocate its destination. The encoder
/// later supplies the directory and manifest bytes.
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

/// Sink interface for receiving the image layout before row writes begin.
template <class p_sink>
concept planned_grouped_soa_generation_sink =
    requires(p_sink& sink, const grouped_soa_storage_plan_view& storage_plan) {
        {
            sink.begin_planned_generation(storage_plan)
        } -> std::same_as<std::expected<void, minimizer_generation_encoding_error>>;
    };

/// Payload row before its address and bytes have a combined CRC.
/// A sink can compute this checksum when accepting the row The default
/// adapter computes it for sinks that only provide write().
struct unchecked_minimizer_generation_row_view {
    index_chunk_address address{};
    std::span<const std::byte> bytes;
};

/// CRC returned after a payload row is accepted. Zero is a valid checksum,
/// so the caller uses expected to distinguish success from failure.
struct minimizer_generation_row_write_receipt {
    std::uint64_t row_checksum = 0;
};

/// Sink interface for  writing payload rows and returning their checksums.
template <class p_sink>
concept grouped_soa_minimizer_payload_sink = requires(
    p_sink& sink, const unchecked_minimizer_generation_row_view& row) {
    {
        sink.write_grouped_soa_payload(row)
    } -> std::same_as<
        std::expected<minimizer_generation_row_write_receipt, minimizer_generation_encoding_error>>;
};

/// Borrowed sink callbacks, with optional checksum calculation during payload writes.
struct minimizer_generation_row_sink_ref {
    void* context = nullptr;
    std::expected<void, minimizer_generation_encoding_error> (*write_fn)(
        void*, const minimizer_generation_row_view&) = nullptr;
    std::expected<minimizer_generation_row_write_receipt, minimizer_generation_encoding_error> (
        *write_grouped_soa_payload_fn)(void*,
                                       const unchecked_minimizer_generation_row_view&) = nullptr;

    /// Send a checksummed row to the sink, or return row_count for an empty binding.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    write(const minimizer_generation_row_view& row) const {
        if (context == nullptr || write_fn == nullptr) {
            return std::unexpected(minimizer_generation_encoding_error::row_count);
        }
        return write_fn(context, row);
    }

    /// Let the payload callback write the row and return its CRC when available.
    /// Otherwise compute the address-bound CRC here and call write().
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

/// Encoded active-head or maniest row with its address and checksum.
struct encoded_minimizer_fixed_row {
    index_chunk_address address{};
    std::array<std::byte, minimizer_generation_manifest::encoded_bytes> storage{};
    std::uint32_t byte_count = 0;
    std::uint64_t row_checksum = 0;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return std::span<const std::byte>{storage.data(), byte_count};
    }
};

/// Encoded generation metadata. Payload bytes have already been sent to the sink.
struct encoded_grouped_soa_generation {
    minimizer_generation_head head{};
    minimizer_generation_manifest manifest{};
    /// Active-head row returned to the caller for later publication.
    /// encode_minimizer_generation never sends it  to the row sink.
    encoded_minimizer_fixed_row unpublished_head_row{};
    encoded_minimizer_fixed_row manifest_row{};
    grouped_soa_directory_view directory{};
};

/// Generation and source identity attached to execution stats before publication.
/// The checksums distinguish encoded generations that used the same backend
/// and target length but have different source data or metadata.
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

/// Build publication identity from the encoded generation metadata and checksums.
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

/// Directory plan, event count, and checksum that later producer scans must match.
struct grouped_soa_generation_plan {
    grouped_soa_minimizer_profile profile{};
    minimizer_producer_identity producer_backend{};
    std::uint64_t position_segment_span_bp = 0;
    std::uint32_t position_segment_count = 0;
    std::uint64_t canonical_event_count = 0;
    std::uint64_t canonical_event_checksum = 0;
    std::vector<grouped_soa_bucket_directory_entry> entries;
};

/// Borrowed plan offered before payload emission.
/// Referenced objects stay valid, for the callback. Extent  and payload checksums
/// are still unset A direct sink can allocate its destination and copy rows
/// there, but must wait for matching finalized metadata before publishing it.
struct direct_grouped_soa_generation_plan_view {
    const grouped_soa_storage_plan_view& storage_plan;
    const grouped_soa_generation_options& options;
    const grouped_soa_generation_plan& generation_plan;
};

/// Direct sink callbacks for preparing final storage and sealing the generation.
/// Directory and manifest rows still arrive through write(). Seal runs afte
/// all rows and finalized metadata are available.
template <class p_sink>
concept direct_grouped_soa_generation_sink = requires(
    p_sink& sink,
    const direct_grouped_soa_generation_plan_view& plan,
    const encoded_grouped_soa_generation& generation) {
    {
        sink.begin_direct_grouped_soa_generation(plan)
    } -> std::same_as<std::expected<void, minimizer_generation_encoding_error>>;
    {
        sink.seal_direct_grouped_soa_generation(generation)
    } -> std::same_as<std::expected<void, minimizer_generation_encoding_error>>;
};

} // namespace bioseq::indexing
