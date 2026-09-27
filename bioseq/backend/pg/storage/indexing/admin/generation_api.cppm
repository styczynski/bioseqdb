/**
 * @file admin/generation_api.cppm
 * @brief Test helpers for publishing and reading minimizer index generations.
 */
module;

extern "C" {
#include "postgres.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

export module bioseq.postgres.storage.indexing.admin:generation_api;

export import :audit_api;
import bioseq.indexing.minimizer_hashmap;
import bioseq.postgres.storage.indexing.minimizer_read_stats;
import bioseq.sequences.kmers;
import bioseq.storage.layout;

export namespace bioseq::storage::indexing {

/// Report an unsupported test backend name as a PostgreSQL error.
[[noreturn]] void ready_minimizer_fixture_backend_error_as_cpp(std::string_view name);

/** Parse an exact minimizer backend name, raising an error for an unknown name. */
[[nodiscard]] inline bioseq::minimizers::minimizer_sketch_backend
ready_minimizer_fixture_backend_from_name(std::string_view name) {
    using backend_type = bioseq::minimizers::minimizer_sketch_backend;
    if (name == "scalar")
        return backend_type::scalar;
    if (name == "exact_simd")
        return backend_type::exact_simd;
    if (name == "bounded_nthash_simd")
        return backend_type::bounded_nthash_simd;
    if (name == "vulkan_segment")
        return backend_type::vulkan_segment;
    ready_minimizer_fixture_backend_error_as_cpp(name);
    pg_unreachable();
}

/** Publication result and metadata read back after a coordinator test build. */
struct ready_minimizer_generation_coordinator_fixture_result {
    std::string table_name;
    std::string selected_backend_name;
    std::int16_t column_attno = 0;
    std::uint64_t sequence_rowno = 0;
    sequence_locator locator{};
    std::uint8_t alphabet_code = 0;
    std::uint8_t storage_unit_byte_width = 0;
    std::optional<ready_minimizer_generation_publication> publication;
    minimizer_generation_head head{};
    minimizer_generation_manifest manifest{};
    bool head_manifest_verified = false;

    /** Return the publication. Requires a completed test build. */
    [[nodiscard]] const ready_minimizer_generation_publication&
    sealed_publication() const noexcept;
};

/** Stored generation details and lookup results collected by integration tests. */
struct minimizer_generation_integration_fixture_result {
    std::string table_name;
    bioseq::indexing::minimizer_generation_layout layout =
        bioseq::indexing::minimizer_generation_layout::invalid;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t source_content_checksum = 0;
    std::uint64_t storage_contract_checksum = 0;
    std::uint64_t source_num_fragments = 0;
    std::uint64_t source_total_bp = 0;
    std::uint64_t source_padding_letters = 0;
    std::uint64_t writer_rows = 0;
    std::uint64_t writer_batches = 0;
    std::uint64_t published_head_bytes = 0;
    std::uint64_t control_count = 0;
    std::uint64_t position_count = 0;
    std::uint64_t directory_entries = 0;
    std::uint64_t position_segment_count = 0;
    std::uint64_t control_encoded_bytes = 0;
    std::uint64_t position_encoded_bytes = 0;
    std::uint64_t control_chunks = 0;
    std::uint64_t position_chunks = 0;
    std::uint64_t validated_control_bytes = 0;
    std::uint64_t validated_position_bytes = 0;
    std::uint64_t validated_control_checksum = 0;
    std::uint64_t validated_position_checksum = 0;
    std::uint64_t lookup_count = 0;
    std::uint64_t lookup_bp = 0;
    bool lookup_reverse = false;
    bool positions_complete = false;
    minimizer_generation_read_stats read_stats{};
    minimizer_generation_read_stats selective_probe_stats{};
};

/** Byte counts, checksums, and read counters from a complete payload scan. */
struct minimizer_generation_full_payload_fixture_result {
    std::string table_name;
    std::string selected_backend_name;
    std::uint64_t generation_id = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t control_checksum = 0;
    std::uint64_t position_checksum = 0;
    minimizer_generation_read_stats read_stats{};
};

/** Table identities and cache results observed during a storage lifecycle test. */
struct minimizer_generation_storage_state_fixture_result {
    Oid before_oid = InvalidOid;
    Oid operation_oid = InvalidOid;
    Oid after_oid = InvalidOid;
    std::optional<bool> same_handle_rebound;
    std::optional<bool> generation_schema_after;
    std::optional<bool> chunk_cache_hit;
    std::optional<bool> assembled_cache_hit;
};

/** Build and publish a real minimizer index with the selected test backend. */
[[nodiscard]] ready_minimizer_generation_coordinator_fixture_result
build_ready_minimizer_generation_coordinator_fixture_impl(Oid rel_oid,
                                                          std::string_view index_name,
                                                          std::uint64_t sequence_rowno,
                                                          std::string_view backend_name);

/** Publish deterministic minimizer events for grouped-SoA storage tests. */
[[nodiscard]] minimizer_generation_integration_fixture_result
build_minimizer_generation_integration_fixture_impl(
    Oid rel_oid,
    std::string_view index_name,
    std::uint64_t sequence_rowno,
    std::uint16_t kmer_size,
    std::uint16_t window_size,
    bioseq::indexing::minimizer_generation_layout layout,
    bioseq::minimizers::minimizer_sketch_backend backend,
    std::uint64_t position_segment_span_bp);

/** Reopen a grouped-SoA test index and check a lookup and its read statistics. */
[[nodiscard]] minimizer_generation_integration_fixture_result
read_minimizer_generation_integration_fixture_impl(
    Oid rel_oid,
    std::string_view index_name,
    std::uint64_t sequence_rowno,
    std::uint16_t kmer_size,
    std::uint16_t window_size,
    bioseq::indexing::minimizer_generation_layout layout,
    bioseq::minimizers::minimizer_sketch_backend backend);

/** Scan all stored payload bytes once and return their counts and checksums. */
[[nodiscard]] minimizer_generation_full_payload_fixture_result
read_minimizer_generation_full_payload_fixture_impl(
    Oid rel_oid,
    std::string_view index_name,
    std::uint64_t sequence_rowno,
    std::uint16_t kmer_size,
    std::uint16_t window_size,
    bioseq::indexing::minimizer_generation_layout layout,
    bioseq::minimizers::minimizer_sketch_backend backend);

/** Invalidate a source epoch for a failure test using a non-first fragment ordinal. */
void remove_sequence_fragment_impl(Oid rel_oid,
                                   std::string_view index_name,
                                   std::uint64_t sequence_rowno,
                                   std::uint32_t fragment_ordinal);

/** Trigger a named read-session error and check its cleanup path. */
void exercise_minimizer_generation_session_failure_impl(Oid rel_oid,
                                                        std::string_view index_name,
                                                        std::uint64_t sequence_rowno,
                                                        std::string_view mode);

/** Test index-table creation, clearing, rollback, or cache invalidation. */
[[nodiscard]] minimizer_generation_storage_state_fixture_result
exercise_minimizer_generation_storage_state_impl(Oid rel_oid,
                                                 std::string_view index_name,
                                                 std::uint64_t sequence_rowno,
                                                 std::string_view operation);

} // namespace bioseq::storage::indexing
