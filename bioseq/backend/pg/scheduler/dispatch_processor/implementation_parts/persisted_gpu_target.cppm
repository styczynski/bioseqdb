/**
 * @file implementation_parts/persisted_gpu_target.cppm
 * @brief Owns authenticated persisted grouped-SoA GPU target preparation.
 */
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <exception>
#include <expected>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <queue>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

export module bioseq.postgres.scheduler.dispatch_processor:persisted_gpu_target;

import bioseq.postgres.scheduler.dispatch_processor;

import :highway_registry;
import :cpu_preparation;
import :typed_flat_selector;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
import bioseq.common.seqan_adapter;
import bioseq.highway.dispatch_contract;
import bioseq.postgres.support;
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.configuration.core;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.handles;
import bioseq.postgres.storage.indexing.residency;
import bioseq.postgres.storage.indexing.sorted_runs;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.chaining.vulkan;
import bioseq.sequences.cigar;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.gpu.vulkan.runtime;
import bioseq.postgres.types.compact_sequence;
import bioseq.instrumentation.clock;
import bioseq.postgres.storage.manager;
import bioseq.storage.lazy_sequence;
import bioseq.storage.scanner;
import bioseq.postgres.storage.scanner;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.scheduler.supervisor.vulkan;

namespace bioseq::simd {

void batch_processor::prepare_gpu_persisted_grouped_soa_target(
    const Oid database_oid,
    const Oid tablespace_oid,
    const Oid main_relfilenode,
    const Oid user_relid,
    const std::int16_t column_attno,
    const std::string_view index_name,
    const std::uint64_t sequence_rowno,
    const std::uint64_t target_bp_size,
    const std::uint64_t query_count,
    const std::uint8_t alphabet_code,
    const std::uint8_t storage_unit_byte_width,
    const minimap2_parameters& params,
    const bioseq::storage::indexing::borrowed_executor_read_context& executor_read,
    gpu_prepared_grouped_soa_target& prepared_target) {
    using bioseq::indexing::gpu_grouped_soa_ingest_error;
    using bioseq::indexing::minimizer_generation_layout;
    using bioseq::storage::indexing::authorize_minimizer_generation_catalog_source_pg;
    using bioseq::storage::indexing::format_seqidx_name;
    using bioseq::storage::indexing::index_storage_fragment_generation_read_session;
    using bioseq::storage::indexing::minimizer_generation_administration_identity;
    using bioseq::storage::indexing::minimizer_generation_catalog_source_stats;
    using bioseq::storage::indexing::minimizer_generation_read_stats;
    using bioseq::storage::indexing::minimizer_generation_relation_kind;
    using bioseq::storage::indexing::minimizer_generation_source_locator;
    using bioseq::storage::indexing::open_exact_minimizer_generation_read_session_pg;
    const auto fail = [](const int sqlstate, std::string message) -> void {
        bioseq::postgres::support::invoke_pg_as_cpp([sqlstate, &message] {
            ereport(
                ERROR,
                (errcode(sqlstate),
                 errmsg("required persisted GPU grouped-SoA target is unavailable"),
                 errdetail("%s", message.c_str()),
                 errhint(
                     "Publish a matching grouped-SoA READY generation; "
                     "GPU preparation has no CPU, SIMD, recompute, or alternate-index fallback.")));
        });
    };
    prepared_target = gpu_prepared_grouped_soa_target{};
    const minimizer_sketch_backend backend = pipe::target_index_sketch_backend_from_params(params);
    if (database_oid != MyDatabaseId || !OidIsValid(user_relid) || !OidIsValid(tablespace_oid) ||
        !OidIsValid(main_relfilenode) || column_attno <= 0 ||
        sequence_rowno > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ||
        target_bp_size == 0 || query_count == 0 ||
        !pipe::is_persisted_grouped_soa_gpu_preparation_tuple_valid(
            alphabet_code,
            pgseq_supported_alphabets::alphabet_count,
            storage_unit_byte_width,
            backend,
            params) ||
        !executor_read.matches(database_oid, tablespace_oid, main_relfilenode, user_relid)) {
        fail(ERRCODE_INVALID_PARAMETER_VALUE,
             "GPU prepared-target identity, pinned k/w, or all-GPU selector is invalid");
        return;
    }

    minimizer_generation_administration_identity identity{
        .database_oid = database_oid,
        .tablespace_oid = tablespace_oid,
        .main_relfilenode = main_relfilenode,
        .user_relid = user_relid,
        .column_attno = column_attno,
        .index_name = std::string{index_name},
    };
    auto source_authorization =
        authorize_minimizer_generation_catalog_source_pg(identity, sequence_rowno, executor_read);
    const minimizer_generation_source_locator source =
        source_authorization.source_for_diagnostics();
    const minimizer_generation_catalog_source_stats source_stats =
        source_authorization.stats_for_diagnostics();
    if (source.alphabet_code != alphabet_code ||
        source.storage_unit_byte_width != storage_unit_byte_width ||
        source.locator.logical_length != target_bp_size) {
        fail(ERRCODE_DATA_CORRUPTED,
             "authoritative source identity disagrees with the batch join target typmod or length");
        return;
    }
    identity.table_name = format_seqidx_name(index_name,
                                             database_oid,
                                             tablespace_oid,
                                             main_relfilenode,
                                             column_attno,
                                             source.locator.bucket_index,
                                             source.locator.shard_index);
    const auto expected = bioseq::indexing::expected_grouped_soa_generation_identity_for_backend(
        minimizer_generation_layout::grouped_soa_segment_local,
        backend,
        source.alphabet_code,
        source.storage_unit_byte_width,
        static_cast<std::uint16_t>(params.kmer_size),
        static_cast<std::uint16_t>(params.window_size),
        bioseq::indexing::k_grouped_soa_default_max_occurrence_cap);
    if (!expected) {
        fail(ERRCODE_DATA_CORRUPTED,
             fmt::format("GPU grouped-SoA storage contract rejected storage_error={}",
                         static_cast<unsigned>(expected.error())));
        return;
    }
    auto opened = open_exact_minimizer_generation_read_session_pg(
        identity.table_name, index_name, sequence_rowno, executor_read, *expected);
    if (!opened) {
        const int sqlstate = opened.failure.kind == minimizer_generation_relation_kind::missing
                                 ? ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE
                                 : ERRCODE_DATA_CORRUPTED;
        fail(sqlstate,
             fmt::format("generation relation {} rejected kind={} schema_error={}",
                         identity.table_name,
                         static_cast<unsigned>(opened.failure.kind),
                         static_cast<unsigned>(opened.failure.schema_error)));
        return;
    }
    index_storage_fragment_generation_read_session& session = *opened.session;
    const auto head = session.read_active_head_once();
    const auto& metadata = session.read_manifest_directory_once(head);
    const auto manifest = metadata.manifest;
    session.consume_source_authorization(std::move(source_authorization));

    auto& runtime = bioseq::postgres::scheduler::ensure_pg_vulkan_runtime_ready();
    std::uint64_t cold_load_identity = 14695981039346656037ull;
    cold_load_identity = bioseq::common::hash_combine_64(cold_load_identity, head.generation_id);
    cold_load_identity = bioseq::common::hash_combine_64(cold_load_identity, head.source_identity);
    cold_load_identity =
        bioseq::common::hash_combine_64(cold_load_identity, head.manifest_checksum);
    cold_load_identity =
        bioseq::common::hash_combine_64(cold_load_identity, manifest.directory_checksum);
    cold_load_identity =
        bioseq::common::hash_combine_64(cold_load_identity, manifest.payload_checksum);
    cold_load_identity = bioseq::common::hash_combine_64(cold_load_identity, sequence_rowno);
    for (const std::uint8_t byte : runtime.features().device_uuid) {
        cold_load_identity = bioseq::common::hash_combine_64(cold_load_identity, byte);
    }
    if (cold_load_identity == 0u)
        cold_load_identity = 1u;

    auto ingest = bioseq::indexing::gpu_grouped_soa_generation_ingest::begin(
        runtime, head, manifest, *expected, metadata.directory, cold_load_identity);
    if (!ingest) {
        fail(ERRCODE_FEATURE_NOT_SUPPORTED,
             fmt::format("GPU grouped-SoA resident admission rejected foundation_error={}",
                         static_cast<unsigned>(ingest.error())));
        return;
    }
    std::optional<gpu_grouped_soa_ingest_error> callback_error;
    std::exception_ptr callback_exception;
    const auto accept =
        [&callback_error, &callback_exception, &ingest](
            const bioseq::indexing::validated_minimizer_generation_row_receipt& receipt) noexcept {
            if (callback_error || callback_exception)
                return;
            try {
                auto accepted = ingest->accept_validated_row(receipt);
                if (!accepted)
                    callback_error = accepted.error();
            } catch (...) {
                callback_exception = std::current_exception();
            }
        };
    (void)session.read_full_generation_payload_once(accept, accept);
    if (callback_exception)
        std::rethrow_exception(callback_exception);
    if (callback_error) {
        fail(ERRCODE_DATA_CORRUPTED,
             fmt::format("GPU grouped-SoA payload ingest rejected foundation_error={}",
                         static_cast<unsigned>(*callback_error)));
        return;
    }
    auto finished = ingest->finish();
    if (!finished) {
        fail(ERRCODE_DATA_CORRUPTED,
             fmt::format("GPU grouped-SoA resident publication rejected foundation_error={}",
                         static_cast<unsigned>(finished.error())));
        return;
    }
    std::shared_ptr<const bioseq::indexing::gpu_grouped_soa_resident_target> resident =
        std::move(*finished);
    const minimizer_generation_read_stats read_stats = session.finish();
    const auto& resident_identity = resident->identity();
    const bool validation_matches =
        resident->validated() && resident_identity.generation_id == head.generation_id &&
        resident_identity.source_identity == head.source_identity &&
        resident_identity.source_content_checksum == head.source_content_checksum &&
        resident_identity.manifest_checksum == head.manifest_checksum &&
        resident_identity.directory_checksum == manifest.directory_checksum &&
        resident_identity.payload_checksum == manifest.payload_checksum &&
        resident_identity.cold_load_identity == cold_load_identity &&
        resident_identity.device_uuid == runtime.features().device_uuid &&
        manifest.target_length == target_bp_size && manifest.alphabet_code == alphabet_code &&
        manifest.storage_unit_byte_width == storage_unit_byte_width &&
        manifest.gpu_abi_revision == bioseq::indexing::k_grouped_soa_gpu_abi_revision &&
        manifest.max_occ_cap == bioseq::indexing::k_grouped_soa_default_max_occurrence_cap &&
        read_stats.relation_opens == 1 && read_stats.head_scans == 1 &&
        read_stats.manifest_directory_scans == 1 && read_stats.metadata_repeated_reads == 0 &&
        read_stats.full_generation.scan_calls == 1 && read_stats.bulk_read_calls == 1 &&
        read_stats.range_scan_calls == 1 && read_stats.index_scans == 3 &&
        read_stats.payload.delivered_chunks == read_stats.payload.heap_rows_read &&
        read_stats.payload.delivered_bytes == manifest.payload_bytes &&
        read_stats.cold.route_empty == 0 && read_stats.cold.route_selective == 0 &&
        read_stats.cold.route_full == 1 && read_stats.source_parent_rows_read == 1 &&
        read_stats.source_catalog_rows_read == 1 && read_stats.source_catalog_index_scans == 1 &&
        read_stats.source_fragment_rows_read == 0 && read_stats.source_decoded_bp == 0 &&
        source_stats.timed_equation_holds();
    if (!validation_matches) {
        fail(
            ERRCODE_DATA_CORRUPTED,
            "GPU grouped-SoA READY identity, read-work, upload, or source authorization disagrees");
        return;
    }

    prepared_target.target_rowno = static_cast<std::int64_t>(sequence_rowno);
    prepared_target.target_bp_size = target_bp_size;
    prepared_target.query_count = query_count;
    prepared_target.persisted_read_stats = read_stats;
    prepared_target.resident = std::move(resident);
    prepared_target.resident_view = bioseq::gpu::vulkan::gpu_grouped_soa_resident_view{
        .descriptors = &prepared_target.resident->descriptors(),
        .controls = &prepared_target.resident->controls(),
        .positions = &prepared_target.resident->positions(),
        .descriptor_bytes = prepared_target.resident->descriptor_bytes(),
        .control_bytes = prepared_target.resident->control_bytes(),
        .position_bytes = prepared_target.resident->position_bytes(),
        .position_count = prepared_target.resident->position_count(),
        .max_occ_cap = bioseq::indexing::k_grouped_soa_default_max_occurrence_cap,
        .generation_id = resident_identity.generation_id,
        .source_identity = resident_identity.source_identity,
        .cold_load_identity = resident_identity.cold_load_identity,
        .partition_count = prepared_target.resident->descriptor_count(),
        .abi_revision = resident_identity.gpu_abi_revision,
        .device_uuid = resident_identity.device_uuid,
    };
    if (!prepared_target.ready()) {
        fail(ERRCODE_DATA_CORRUPTED,
             "GPU grouped-SoA prepared-target view failed its final ownership check");
    }
}

} // namespace bioseq::simd
