/**
 * @file minimizer_hash_major_diagnostics.cppm
 * @brief Hash-major generation inspection and parallel transport probe SQL routines.
 */
module;

extern "C" {
#include "postgres.h"

#include "utils/errcodes.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <new>

module bioseq.postgres.extension;

import std;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.postgres.support;
import bioseq.postgres.storage.indexing.admin;
import bioseq.postgres.storage.indexing.admin.parallel_scan_probe_boundary;
import bioseq.postgres.storage.indexing.hash_major_parallel_probe;

namespace {

using bioseq::pg::detail::fixed_hex_bytes;
using bioseq::pg::detail::fixed_hex_u64;

[[nodiscard]] auto hash_major_minimizer_generation_inspection_json(
    const bioseq::storage::indexing::hash_major_minimizer_generation_inspection& inspection)
    -> std::string {
    std::string result =
        R"({"schema":"bioseq.hash_major_minimizer_generation_inspection","revision":1,"layout":"hash_major")";
    result.reserve(4'096);
    const auto append = [&result](const std::string_view key, const std::uint64_t value) {
        result += ",\"";
        result.append(key);
        result += "\":";
        result += std::to_string(value);
    };
    const auto append_string = [&result](const std::string_view key, const std::string_view value) {
        result += ",\"";
        result.append(key);
        result += "\":\"";
        result.append(value);
        result += '"';
    };
    const auto bounded_name =
        []<std::size_t p_capacity>(const std::array<char, p_capacity>& value) -> std::string_view {
        const auto end = std::find(value.begin(), value.end(), '\0');
        return std::string_view{value.data(), static_cast<std::size_t>(end - value.begin())};
    };
    const auto stream_kind_name =
        [](const bioseq::indexing::hash_major_minimizer_stream_kind kind) -> std::string_view {
        switch (kind) {
        case bioseq::indexing::hash_major_minimizer_stream_kind::directory:
            return "directory";
        case bioseq::indexing::hash_major_minimizer_stream_kind::controls:
            return "controls";
        case bioseq::indexing::hash_major_minimizer_stream_kind::postings:
            return "postings";
        }
        return "invalid";
    };

    append("database_oid", inspection.database_oid);
    append("main_relfilenode", inspection.main_relfilenode);
    append("column_attno", static_cast<std::uint64_t>(inspection.column_attno));
    append("query_key", inspection.query_key);
    append_string("table_name", bounded_name(inspection.table_name));
    append_string("index_name", bounded_name(inspection.index_name));

    const auto& source = inspection.source;
    result += R"(,"source":{"sequence_rowno":)";
    result += std::to_string(source.sequence_rowno);
    result += R"(,"locator_revision":)";
    result += std::to_string(source.locator.payload_version);
    result += R"(,"bucket_idx":)";
    result += std::to_string(source.locator.bucket_index);
    result += R"(,"shard_idx":)";
    result += std::to_string(source.locator.shard_index);
    result += R"(,"num_fragments":)";
    result += std::to_string(source.locator.fragment_count);
    result += R"(,"total_bp_size":)";
    result += std::to_string(source.locator.logical_length);
    result += R"(,"first_sub_row_no":)";
    result += std::to_string(source.locator.first_fragment_row);
    result += R"(,"source_epoch":)";
    result += std::to_string(source.locator.source_epoch);
    result += R"(,"padding_letters":)";
    result += std::to_string(source.padding_letters);
    result += R"(,"alphabet_code":)";
    result += std::to_string(source.alphabet_code);
    result += R"(,"storage_unit_byte_width":)";
    result += std::to_string(source.storage_unit_byte_width);
    result += R"(,"source_content_crc64":")";
    result += fixed_hex_u64(inspection.source_content_crc64);
    result += R"(","source_identity":")";
    result += fixed_hex_bytes(inspection.source_identity);
    result += R"("})";

    const auto& head = inspection.head;
    result += R"(,"head":{"generation_id":)";
    result += std::to_string(head.generation_id);
    result += R"(,"query_key":)";
    result += std::to_string(head.query_key);
    result += R"(,"source_epoch":)";
    result += std::to_string(head.source_epoch);
    result += R"(,"source_identity":")";
    result += fixed_hex_bytes(head.source_identity);
    result += R"(","source_content_crc64":")";
    result += fixed_hex_u64(head.source_content_crc64);
    result += R"(","compatibility_digest":")";
    result += fixed_hex_bytes(head.storage_profile_digest);
    result += R"(","manifest_address_bound_row_crc64":")";
    result += fixed_hex_u64(head.manifest_address_bound_row_crc64);
    result += R"(","manifest_payload_crc64":")";
    result += fixed_hex_u64(head.manifest_payload_crc64);
    result += R"(","head_crc64":")";
    result += fixed_hex_u64(head.head_crc64);
    result += R"(","dense_seal_present":)";
    result += head.dense_seal.has_value() ? "true" : "false";
    result += '}';

    const auto& manifest = inspection.manifest;
    const auto& storage_profile = manifest.storage_profile;
    result += R"(,"manifest":{"generation_id":)";
    result += std::to_string(manifest.generation_id);
    result += R"(,"source_epoch":)";
    result += std::to_string(manifest.source_epoch);
    result += R"(,"source_identity":")";
    result += fixed_hex_bytes(manifest.source_identity);
    result += R"(","source_content_crc64":")";
    result += fixed_hex_u64(manifest.source_content_crc64);
    result += R"(","target_length":)";
    result += std::to_string(manifest.target_length);
    result += R"(,"unique_hash_count":)";
    result += std::to_string(manifest.unique_hash_count);
    result += R"(,"total_posting_count":)";
    result += std::to_string(manifest.total_posting_count);
    result += R"(,"compatibility_digest":")";
    result += fixed_hex_bytes(manifest.storage_profile_digest);
    result += R"(","authenticated_directory_crc64":")";
    result += fixed_hex_u64(manifest.authenticated_directory_crc64);
    result += R"(","generation_crc64":")";
    result += fixed_hex_u64(manifest.generation_crc64);
    result += R"(","total_control_bytes":)";
    result += std::to_string(manifest.total_control_bytes);
    result += R"(,"total_posting_bytes":)";
    result += std::to_string(manifest.total_posting_bytes);
    result += R"(,"directory_entry_count":)";
    result += std::to_string(manifest.directory_entry_count);
    result += R"(,"compatibility":{"hash_scheme_identity":)";
    result += std::to_string(storage_profile.hash_scheme_identity);
    result += R"(,"hash_scheme_revision":)";
    result += std::to_string(storage_profile.hash_scheme_revision);
    result += R"(,"alphabet_code":)";
    result += std::to_string(storage_profile.alphabet_code);
    result += R"(,"kmer_size":)";
    result += std::to_string(storage_profile.kmer_size);
    result += R"(,"window_size":)";
    result += std::to_string(storage_profile.window_size);
    result += R"(,"hash_width_bytes":)";
    result += std::to_string(storage_profile.hash_width_bytes);
    result += R"(,"stored_posting_width_bytes":)";
    result += std::to_string(storage_profile.stored_posting_width_bytes);
    result += R"(,"position_encoding_revision":)";
    result += std::to_string(storage_profile.position_encoding_revision);
    result += R"(,"hash_semantics_revision":)";
    result += std::to_string(storage_profile.hash_semantics_revision);
    result += R"(,"count_semantics_revision":)";
    result += std::to_string(storage_profile.count_semantics_revision);
    result += R"(,"producer_backend_identity":)";
    result += std::to_string(storage_profile.producer_backend_identity);
    result += R"(,"producer_backend_revision":)";
    result += std::to_string(storage_profile.producer_backend_revision);
    result += R"(,"controls_per_block":)";
    result += std::to_string(storage_profile.controls_per_block);
    result += R"(,"hash_seed":)";
    result += std::to_string(storage_profile.hash_seed);
    result += R"(}})";

    result += R"(,"metadata":{"head_rows":)";
    result += std::to_string(inspection.head_rows);
    result += R"(,"head_bytes":)";
    result += std::to_string(inspection.head_bytes);
    result += R"(,"manifest_rows":)";
    result += std::to_string(inspection.manifest_rows);
    result += R"(,"manifest_bytes":)";
    result += std::to_string(inspection.manifest_bytes);
    result += '}';

    result += R"(,"streams":[)";
    for (std::size_t index = 0; index < inspection.streams.size(); ++index) {
        if (index != 0u)
            result += ',';
        const auto& stream = inspection.streams[index];
        result += R"({"kind":")";
        result.append(stream_kind_name(stream.kind));
        result += R"(","element_count":)";
        result += std::to_string(stream.element_count);
        result += R"(,"logical_bytes":)";
        result += std::to_string(stream.logical_bytes);
        result += R"(,"row_count":)";
        result += std::to_string(stream.row_count);
        result += R"(,"stream_crc64":")";
        result += fixed_hex_u64(stream.stream_crc64);
        result += R"(","exact_catalog_rows":)";
        result += std::to_string(stream.exact_catalog_rows);
        result += R"(,"exact_catalog_bytes":)";
        result += std::to_string(stream.exact_catalog_bytes);
        result += R"(,"exact_catalog_stream_crc64":")";
        result += fixed_hex_u64(stream.exact_catalog_stream_crc64);
        result += R"("})";
    }
    result += "]}";
    return result;
}

[[nodiscard]] auto hash_major_minimizer_parallel_scan_stats_json(
    const bioseq::storage::indexing::hash_major_minimizer_parallel_scan_stats& stats)
    -> std::string {
    std::string result =
        R"({"schema":"bioseq_internal.hash_major_minimizer_parallel_scan_probe","revision":1)";
    result.reserve(768);
    const auto append = [&result](const std::string_view key, const std::uint64_t value) {
        result += ",\"";
        result.append(key);
        result += "\":";
        result += std::to_string(value);
    };
    append("requested_workers", stats.requested_workers);
    append("launched_workers", stats.launched_workers);
    append("generation_id", stats.generation_id);
    append("query_key", stats.query_key);
    append("expected_rows", stats.expected_rows);
    append("scanned_rows", stats.scanned_rows);
    append("expected_bytes", stats.expected_bytes);
    append("scanned_bytes", stats.scanned_bytes);
    append("duplicate_rows", stats.duplicate_rows);
    append("missing_rows", stats.missing_rows);
    append("corrupt_rows", stats.corrupt_rows);
    append("elapsed_ns", stats.elapsed_ns);
    result += R"(,"checksum_digest":")";
    result += fixed_hex_u64(stats.checksum_digest);
    result += R"(","receipt_digest":")";
    result += fixed_hex_u64(stats.receipt_digest);
    result += R"(","has_leader_participation":)";
    result += stats.has_leader_participation ? "true" : "false";
    result += '}';
    return result;
}

} // namespace

namespace bioseq::pg::sql_routines {

/**
 * @brief Validates a source-bound ready hash-major generation and verifies its
 * metadata, catalog rows, byte counts, and checksums.
 */
bioseq::postgres::support::jsonb_value inspect_ready_hash_major_minimizer_generation(
    Oid rel, std::string index_name, std::int64_t query_key) {
    if (query_key < 0) {
        bioseq::postgres::support::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {.message = "bioseq hash-major inspection query key is negative"});
    }
    const auto inspection =
        bioseq::storage::indexing::inspect_hash_major_minimizer_generation_cold_boundary(
            rel, index_name, static_cast<std::uint64_t>(query_key));
    return bioseq::postgres::support::jsonb_value{
        hash_major_minimizer_generation_inspection_json(inspection)};
}

/**
 * @brief Validates a ready generation row, plain payload, and address checksum
 * using a serial scan when requested_workers is zero or a shared parallel heap
 * scan when it is positive.
 */
bioseq::postgres::support::jsonb_value
hash_major_minimizer_parallel_scan_probe(Oid source_table,
                                         std::string index_name,
                                         std::int64_t sequence_rowno,
                                         std::int32_t requested_workers) {
    if (sequence_rowno < 0 || requested_workers < 0 || requested_workers > 64) {
        bioseq::postgres::support::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {.message = "bioseq hash-major parallel scan probe parameters are invalid"});
    }
    const auto stats =
        bioseq::storage::indexing::hash_major_minimizer_parallel_scan_probe_cold_boundary(
            source_table,
            index_name,
            static_cast<std::uint64_t>(sequence_rowno),
            static_cast<std::uint32_t>(requested_workers));
    return bioseq::postgres::support::jsonb_value{
        hash_major_minimizer_parallel_scan_stats_json(stats)};
}

} // namespace bioseq::pg::sql_routines
