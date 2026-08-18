/**
 * @file admin/generation_api.cppm
 * @brief Declares PostgreSQL generation integration fixture entry points.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.storage.indexing.admin:generation_api;

import std;
export import :audit_api;
import bioseq.indexing.minimizer_hashmap;
import bioseq.postgres.storage.indexing.minimizer_read_stats;
import bioseq.sequences.kmers;
import bioseq.storage.layout;

export namespace bioseq::storage::indexing {

/** Result of publishing and reopening one grouped-SoA coordinator fixture. */
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

    /** Return the sealed publication retained by a completed fixture. */
    [[nodiscard]] const ready_minimizer_generation_publication&
    sealed_publication() const noexcept;
};

/** Observations returned by a live grouped-SoA generation fixture. */
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

/** Validation result for a single-scan full-generation payload read. */
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

/** Observations for generation storage lifecycle and residency transitions. */
struct minimizer_generation_storage_state_fixture_result {
    Oid before_oid = InvalidOid;
    Oid operation_oid = InvalidOid;
    Oid after_oid = InvalidOid;
    std::optional<bool> same_handle_rebound;
    std::optional<bool> generation_schema_after;
    std::optional<bool> chunk_cache_hit;
    std::optional<bool> assembled_cache_hit;
};

/** Publish one READY generation through the selected producer backend. */
[[nodiscard]] ready_minimizer_generation_coordinator_fixture_result
build_ready_minimizer_generation_coordinator_fixture_impl(Oid rel_oid,
                                                          std::string_view index_name,
                                                          std::uint64_t sequence_rowno,
                                                          std::string_view backend_name);

/** Build and publish one deterministic grouped-SoA integration fixture. */
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

/** Reopen and validate one deterministic grouped-SoA integration fixture. */
[[nodiscard]] minimizer_generation_integration_fixture_result
read_minimizer_generation_integration_fixture_impl(
    Oid rel_oid,
    std::string_view index_name,
    std::uint64_t sequence_rowno,
    std::uint16_t kmer_size,
    std::uint16_t window_size,
    bioseq::indexing::minimizer_generation_layout layout,
    bioseq::minimizers::minimizer_sketch_backend backend);

/** Stream and validate the complete opaque payload of one fixture generation. */
[[nodiscard]] minimizer_generation_full_payload_fixture_result
read_minimizer_generation_full_payload_fixture_impl(
    Oid rel_oid,
    std::string_view index_name,
    std::uint64_t sequence_rowno,
    std::uint16_t kmer_size,
    std::uint16_t window_size,
    bioseq::indexing::minimizer_generation_layout layout,
    bioseq::minimizers::minimizer_sketch_backend backend);

/** Remove one non-first physical source fragment for failure testing. */
void remove_sequence_fragment_impl(Oid rel_oid,
                                   std::string_view index_name,
                                   std::uint64_t sequence_rowno,
                                   std::uint32_t fragment_ordinal);

/** Exercise a named read-session failure transition against a live fixture. */
void exercise_minimizer_generation_session_failure_impl(Oid rel_oid,
                                                        std::string_view index_name,
                                                        std::uint64_t sequence_rowno,
                                                        std::string_view mode);

/** Exercise one generation storage lifecycle operation. */
[[nodiscard]] minimizer_generation_storage_state_fixture_result
exercise_minimizer_generation_storage_state_impl(Oid rel_oid,
                                                 std::string_view index_name,
                                                 std::uint64_t sequence_rowno,
                                                 std::string_view operation);

} // namespace bioseq::storage::indexing
