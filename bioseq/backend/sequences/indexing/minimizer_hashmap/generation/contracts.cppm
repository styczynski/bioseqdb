/**
 * @file minimizer_hashmap/generation/contracts.cppm
 * @brief Declares grouped-SoA generation inputs, sinks, plans, and results.
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
    /// Registry-selected requested/effective algorithm and physical record identity.
    minimizer_generation_algorithm_identity algorithm_identity{};
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

    /// Forward one event to the bound replay callback.
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

    /// Forward one checksummed row to the bound generation sink.
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

/// Callback-scoped shape available before grouped-SoA payload emission.
///
/// Extent checksums and the aggregate payload checksum do not exist yet. The
/// encoder owns every referenced object for the complete callback. A direct
/// sink may allocate its final destination and copy each payload row
/// immediately, but must not publish that destination before the matching
/// finalized generation is delivered to its seal callback.
struct direct_grouped_soa_generation_plan_view {
    const grouped_soa_storage_plan_view& storage_plan;
    const grouped_soa_generation_options& options;
    const grouped_soa_generation_plan& generation_plan;
};

/// Optional sink handshake for direct payload-to-final-owner generation.
/// Implementations still receive ordinary row callbacks for directory and
/// manifest rows; the terminal callback is the only publication boundary.
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
