/**
 * @file implementation_parts/persisted_gpu_target.cppm
 * @brief Prepare compatible stored MPF and MPS indexes for Vulkan seeding.
 */
module;

#include <fmt/format.h>

extern "C" {
#include "postgres.h"
// Include PostgreSQL 18+ tuple-descriptor helpers before  the catalog modules.
#include "access/tupdesc.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "utils/errcodes.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

module bioseq.postgres.scheduler.dispatch_processor;

import bioseq.common.hashing;
import bioseq.common.logging;
import bioseq.gpu.vulkan.runtime;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.indexing.minimizer_hashmap;
import bioseq.pipeline.minimizer_execution_contract;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.minimizer_fragment_mapping_lease;
import bioseq.pipeline.pipe;
import bioseq.parallelism.memory_admission;
import bioseq.postgres.scheduler.supervisor.vulkan;
import bioseq.postgres.support;
import bioseq.postgres.types.compact_sequence;

namespace bioseq::simd {

namespace {

/** Check the selected GPU seed configuration. Chaining and alignment are checked separately. */
[[nodiscard]] bool selected_gpu_seed_preparation_tuple_valid(
    const std::uint8_t alphabet_code,
    const std::uint8_t storage_unit_byte_width,
    const minimizer_sketch_backend backend,
    const bioseq::indexing::minimizer_generation_algorithm_identity& algorithm_identity,
    const minimap2_parameters& params) noexcept {
    const auto effective_backend =
        bioseq::indexing::minimizer_sketch_backend_for_effective_algorithm(
            algorithm_identity.effective_algorithm);
    const auto producer =
        bioseq::indexing::minimizer_producer_backend_identity_for_backend(backend);
    return static_cast<std::size_t>(alphabet_code) < pgseq_supported_biological_alphabets::alphabet_count &&
           (storage_unit_byte_width == 4u || storage_unit_byte_width == 8u) &&
           params.kmer_size > 0 &&
           params.kmer_size <= static_cast<int>((std::numeric_limits<std::uint16_t>::max)()) &&
           params.window_size > 0 &&
           params.window_size <= static_cast<int>((std::numeric_limits<std::uint16_t>::max)()) &&
           algorithm_identity.requested_algorithm == params.minimizer_algorithm &&
           algorithm_identity.alphabet_id == alphabet_code &&
           algorithm_identity.packed_storage_bits == storage_unit_byte_width * 8u &&
           algorithm_identity.runtime_kmer_size == static_cast<std::uint16_t>(params.kmer_size) &&
           algorithm_identity.runtime_window_size ==
               static_cast<std::uint16_t>(params.window_size) &&
           effective_backend && *effective_backend == backend &&
           bioseq::indexing::is_valid_minimizer_generation_algorithm_identity(
               algorithm_identity, producer) &&
           params.max_occ >= 0 && params.vulkan_enabled &&
           params.vulkan_seed_hits_enabled &&
           params.minimizer_algorithm == minimizer_algorithm::vulkan_minimap_integer &&
           backend == minimizer_sketch_backend::vulkan_segment;
}

} // namespace

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
    pipe::statement_execution_plan_owner execution_plan,
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
    if (!execution_plan ||
        index_name != bioseq::indexing::k_minimizers_per_sequence_index_name) {
        fail(ERRCODE_INVALID_PARAMETER_VALUE,
             "GPU prepared-target preparation has no immutable statement or exact MPS identity");
        return;
    }
    const minimap2_parameters& params = execution_plan->frozen_parameters;
    if (database_oid != MyDatabaseId || !OidIsValid(user_relid) ||
        !OidIsValid(tablespace_oid) ||
        !OidIsValid(main_relfilenode) || column_attno <= 0 ||
        sequence_rowno > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ||
        target_bp_size == 0 || query_count == 0 || params.kmer_size <= 0 ||
        params.kmer_size > static_cast<int>((std::numeric_limits<std::uint16_t>::max)()) ||
        params.window_size <= 0 ||
        params.window_size > static_cast<int>((std::numeric_limits<std::uint16_t>::max)()) ||
        !executor_read.matches(database_oid, tablespace_oid, main_relfilenode, user_relid)) {
        fail(ERRCODE_INVALID_PARAMETER_VALUE,
             "GPU prepared-target identity, pinned k/w, or selected seed route is invalid");
        return;
    }
    const std::uint16_t runtime_kmer_size = static_cast<std::uint16_t>(params.kmer_size);
    const std::uint16_t runtime_window_size = static_cast<std::uint16_t>(params.window_size);

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
    const std::uint64_t algorithm_position_extent_bp =
        index_name == bioseq::indexing::k_minimizers_per_fragment_index_name
            ? bioseq::indexing::k_minimizer_fragment_algorithm_position_extent_bp
            : target_bp_size;
    const auto algorithm_identity =
        bioseq::indexing::select_minimizer_generation_algorithm_identity_runtime(
            params.minimizer_algorithm,
            algorithm_position_extent_bp,
            source.alphabet_code,
            static_cast<std::uint8_t>(source.storage_unit_byte_width * 8u),
            runtime_kmer_size,
            runtime_window_size);
    if (!algorithm_identity) {
        fail(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
             "generated GPU READY minimizer algorithm identity is unavailable");
        return;
    }
    const auto selected_backend =
        bioseq::indexing::minimizer_sketch_backend_for_effective_algorithm(
            algorithm_identity->effective_algorithm);
    if (!selected_backend ||
        !bioseq::indexing::is_valid_minimizer_generation_algorithm_identity(
            *algorithm_identity,
            bioseq::indexing::minimizer_producer_backend_identity_for_backend(
                *selected_backend))) {
        fail(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
             "generated GPU READY minimizer effective producer is unavailable");
        return;
    }
    const minimizer_sketch_backend backend = *selected_backend;
    if (!selected_gpu_seed_preparation_tuple_valid(source.alphabet_code,
                                                   source.storage_unit_byte_width,
                                                   backend,
                                                   *algorithm_identity,
                                                   params)) {
        fail(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
             "generated READY identity is not executable by the selected GPU seed route");
        return;
    }
    const auto expected = bioseq::indexing::expected_grouped_soa_generation_identity_for_backend(
        minimizer_generation_layout::grouped_soa_segment_local,
        backend,
        *algorithm_identity,
        source.alphabet_code,
        source.storage_unit_byte_width,
        runtime_kmer_size,
        runtime_window_size,
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
        manifest.algorithm_identity == *algorithm_identity &&
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

    prepared_target.execution_plan = std::move(execution_plan);
    prepared_target.target_rowno = static_cast<std::int64_t>(sequence_rowno);
    prepared_target.target_bp_size = target_bp_size;
    prepared_target.query_count = query_count;
    prepared_target.persisted = true;
    prepared_target.persisted_read_stats = read_stats;
    prepared_target.operation_identity = cold_load_identity;
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
    prepared_target.canonical_empty = prepared_target.resident_view.position_count == 0u;
    if (!prepared_target.ready()) {
        fail(ERRCODE_DATA_CORRUPTED,
             "GPU grouped-SoA prepared-target view failed its final ownership check");
    }
}

void batch_processor::prepare_gpu_persisted_mpf_target(
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
    pipe::statement_execution_plan_owner execution_plan,
    const bioseq::storage::indexing::borrowed_executor_read_context& executor_read,
    gpu_prepared_mpf_target& prepared_target) {
    using bioseq::indexing::minimizer_generation_layout;
    using bioseq::indexing::prepared_minimizer_fragment_reader;
    using bioseq::indexing::prepared_minimizer_fragment_reader_mode;
    using bioseq::storage::indexing::authorize_minimizer_generation_catalog_source_pg;
    using bioseq::storage::indexing::format_seqidx_name;
    using bioseq::storage::indexing::index_storage_fragment_generation_read_session;
    using bioseq::storage::indexing::minimizer_fragment_image_mapped_open_validation;
    using bioseq::storage::indexing::minimizer_generation_administration_identity;
    using bioseq::storage::indexing::minimizer_generation_catalog_source_stats;
    using bioseq::storage::indexing::minimizer_generation_read_metadata;
    using bioseq::storage::indexing::minimizer_generation_read_stats;
    using bioseq::storage::indexing::minimizer_generation_relation_kind;
    using bioseq::storage::indexing::minimizer_generation_source_locator;
    using bioseq::storage::indexing::open_exact_minimizer_fragment_image_pg;
    using bioseq::storage::indexing::open_exact_minimizer_generation_read_session_pg;

    const auto fail = [](const int sqlstate, std::string message) -> void {
        bioseq::postgres::support::invoke_pg_as_cpp([sqlstate, &message] {
            ereport(ERROR,
                    (errcode(sqlstate),
                     errmsg("required persisted GPU MPF target is unavailable"),
                     errdetail("%s", message.c_str()),
                     errhint("Publish a matching grouped-SoA MPF READY generation; "
                             "this route has no CPU seed or target reconstruction fallback.")));
        });
    };
    prepared_target = gpu_prepared_mpf_target{};
    if (!execution_plan || index_name != bioseq::indexing::k_minimizers_per_fragment_index_name) {
        fail(ERRCODE_INVALID_PARAMETER_VALUE,
             "MPF preparation has no immutable statement or exact logical index identity");
        return;
    }
    const minimap2_parameters& params = execution_plan->frozen_parameters;
    if (database_oid != MyDatabaseId || !OidIsValid(user_relid) ||
        !OidIsValid(tablespace_oid) || !OidIsValid(main_relfilenode) || column_attno <= 0 ||
        sequence_rowno > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ||
        target_bp_size == 0u || query_count == 0u || params.kmer_size <= 0 ||
        params.kmer_size > static_cast<std::int32_t>((std::numeric_limits<std::uint16_t>::max)()) ||
        params.window_size <= 0 ||
        params.window_size > static_cast<std::int32_t>((std::numeric_limits<std::uint16_t>::max)()) ||
        !executor_read.matches(database_oid, tablespace_oid, main_relfilenode, user_relid)) {
        fail(ERRCODE_INVALID_PARAMETER_VALUE,
             "MPF prepared-target identity, runtime k/w, or executor snapshot is invalid");
        return;
    }
    const std::uint16_t runtime_kmer_size = static_cast<std::uint16_t>(params.kmer_size);
    const std::uint16_t runtime_window_size = static_cast<std::uint16_t>(params.window_size);

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
    if (source.sequence_rowno != sequence_rowno ||
        source.locator.logical_length != target_bp_size || source.alphabet_code != alphabet_code ||
        source.storage_unit_byte_width != storage_unit_byte_width) {
        fail(ERRCODE_DATA_CORRUPTED,
             "authoritative MPF source disagrees with target row, typmod, or length");
        return;
    }
    identity.table_name = format_seqidx_name(index_name,
                                             database_oid,
                                             tablespace_oid,
                                             main_relfilenode,
                                             column_attno,
                                             source.locator.bucket_index,
                                             source.locator.shard_index);
    const auto algorithm_identity =
        bioseq::indexing::select_minimizer_generation_algorithm_identity_runtime(
            params.minimizer_algorithm,
            bioseq::indexing::k_minimizer_fragment_algorithm_position_extent_bp,
            source.alphabet_code,
            static_cast<std::uint8_t>(source.storage_unit_byte_width * 8u),
            runtime_kmer_size,
            runtime_window_size);
    if (!algorithm_identity) {
        fail(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
             "MPF generated algorithm identity is unavailable for runtime k/w");
        return;
    }
    const auto selected_backend =
        bioseq::indexing::minimizer_sketch_backend_for_effective_algorithm(
            algorithm_identity->effective_algorithm);
    if (!selected_backend ||
        !bioseq::indexing::is_valid_minimizer_generation_algorithm_identity(
            *algorithm_identity,
            bioseq::indexing::minimizer_producer_backend_identity_for_backend(
                *selected_backend)) ||
        !selected_gpu_seed_preparation_tuple_valid(source.alphabet_code,
                                                   source.storage_unit_byte_width,
                                                   *selected_backend,
                                                   *algorithm_identity,
                                                   params)) {
        fail(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
             "MPF READY producer is not the selected Vulkan seed producer");
        return;
    }
    const auto expected = bioseq::indexing::expected_grouped_soa_generation_identity_for_backend(
        minimizer_generation_layout::grouped_soa_segment_local,
        *selected_backend,
        *algorithm_identity,
        source.alphabet_code,
        source.storage_unit_byte_width,
        runtime_kmer_size,
        runtime_window_size,
        bioseq::indexing::k_grouped_soa_default_max_occurrence_cap);
    if (!expected) {
        fail(ERRCODE_DATA_CORRUPTED,
             fmt::format("MPF grouped-SoA contract rejected storage_error={}",
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
             fmt::format("MPF generation relation {} rejected kind={} schema_error={}",
                         identity.table_name,
                         static_cast<unsigned>(opened.failure.kind),
                         static_cast<unsigned>(opened.failure.schema_error)));
        return;
    }
    index_storage_fragment_generation_read_session& session = *opened.session;
    const auto head = session.read_active_head_once();
    const minimizer_generation_read_metadata& metadata =
        session.read_manifest_directory_once(head);
    const auto manifest = metadata.manifest;
    if (manifest.layout != minimizer_generation_layout::grouped_soa_segment_local ||
        manifest.target_length != target_bp_size || manifest.kmer_size != runtime_kmer_size ||
        manifest.window_size != runtime_window_size || manifest.algorithm_identity !=
                                                            *algorithm_identity) {
        fail(ERRCODE_DATA_CORRUPTED,
             "MPF READY manifest changed layout, target, runtime k/w, or algorithm identity");
        return;
    }
    session.consume_source_authorization(std::move(source_authorization));
    auto mapped_image = open_exact_minimizer_fragment_image_pg(
        identity,
        sequence_rowno,
        head,
        manifest,
        session.read_live_storage_descriptor_once(identity),
        minimizer_fragment_image_mapped_open_validation::full_metadata);
    // Declare the mapping owner before the reader so the reader is destroyed
    // first if later preparation throws.
    bioseq::storage::indexing::mapped_minimizer_fragment_lease_guard mapping_guard;
    auto mapped_owner = mapped_image.borrow_mapping_token();
    auto reader = prepared_minimizer_fragment_reader::open(
        bioseq::parallelism::memory_admission_controller::instance(),
        head,
        manifest,
        *expected,
        metadata.directory.encoded_bytes(),
        static_cast<std::uint64_t>(params.max_occ),
        std::move(mapped_owner),
        mapped_image.payload(),
        std::nullopt,
        prepared_minimizer_fragment_reader_mode::gpu_source_only);
    if (!reader || !reader->gpu_source_ready() || reader->target_length() != target_bp_size) {
        fail(ERRCODE_DATA_CORRUPTED,
             fmt::format("MPF mapped GPU source rejected storage_error={}",
                         reader ? 0u : static_cast<unsigned>(reader.error())));
        return;
    }
    // Save the directory count before finish destroys the, session metadata.
    // The reader already owns its decoded directory
    const std::size_t metadata_directory_entry_count = metadata.directory.entries().size();
    minimizer_generation_read_stats read_stats =
        session.finish_mapped_minimizer_fragment_payload();
    const auto scheme =
        bioseq::indexing::minimizer_scheme_identity_for_backend(*selected_backend);
    const auto producer_backend =
        bioseq::indexing::minimizer_producer_backend_identity_for_backend(*selected_backend);
    const std::uint64_t expected_metadata_rows = 2u + read_stats.directory_chunks_read;
    const std::uint64_t expected_metadata_bytes =
        bioseq::indexing::minimizer_generation_head::encoded_bytes +
        bioseq::indexing::minimizer_manifest_encoded_bytes(manifest.layout) +
        read_stats.directory_bytes_read;
    const bool read_validation_matches =
        reader->gpu_source_ready() && !reader->compact_direct_only() &&
        reader->mapped_grouped_soa_payload_bytes() == manifest.payload_bytes &&
        reader->grouped_soa_directory_entry_count() == metadata_directory_entry_count &&
        reader->target_length() == target_bp_size &&
        manifest.minimizer_scheme_id == scheme.id &&
        manifest.minimizer_scheme_revision == scheme.revision &&
        manifest.producer_backend_id == producer_backend.id &&
        manifest.producer_identity_revision == producer_backend.revision &&
        manifest.max_occ_cap ==
            bioseq::indexing::k_grouped_soa_default_max_occurrence_cap &&
        head.generation_id != 0u && head.source_identity != 0u &&
        read_stats.relation_opens == 1u && read_stats.head_scans == 1u &&
        read_stats.head_rows_read == 1u && read_stats.manifest_directory_scans == 1u &&
        read_stats.manifest_rows_read == 1u &&
        read_stats.directory_chunks_read == manifest.directory_chunk_count &&
        read_stats.directory_bytes_read == manifest.directory_bytes &&
        read_stats.metadata_heap_rows_read == expected_metadata_rows &&
        read_stats.metadata_bytes_read == expected_metadata_bytes &&
        read_stats.total_heap_rows_read == expected_metadata_rows &&
        read_stats.total_bytes_read == expected_metadata_bytes &&
        read_stats.explicit_copy_bytes == expected_metadata_bytes &&
        read_stats.toast_values_read == 0u && read_stats.detoast_bytes == 0u &&
        read_stats.metadata_repeated_reads == 0u &&
        read_stats.full_generation.scan_calls == 0u && read_stats.bulk_read_calls == 0u &&
        read_stats.range_scan_calls == 0u && read_stats.index_scans == 2u &&
        read_stats.payload.delivered_chunks == 0u &&
        read_stats.payload.heap_rows_read == 0u &&
        read_stats.payload.delivered_bytes == 0u && read_stats.payload_bytes_read == 0u &&
        read_stats.cold.route_empty == 0u && read_stats.cold.route_selective == 0u &&
        read_stats.cold.route_full == 0u && read_stats.cold.head_scans == 1u &&
        read_stats.cold.directory_scans == 1u &&
        read_stats.source_parent_rows_read == 1u &&
        read_stats.source_catalog_rows_read == 1u &&
        read_stats.source_catalog_index_scans == 1u &&
        read_stats.source_fragment_rows_read == 0u && read_stats.source_decoded_bp == 0u &&
        source_stats.timed_equation_holds();
    if (!read_validation_matches) {
        fail(ERRCODE_DATA_CORRUPTED,
             "MPF mapped source read-work or source authorization disagrees");
        return;
    }
    mapping_guard = std::move(mapped_image).release_mapping_lease_guard();
    if (!mapping_guard) {
        fail(ERRCODE_DATA_CORRUPTED, "MPF mapped source lost its mmap lifetime authority");
        return;
    }
    // Keep this target's GPU index in its own cache. A zero byte limit disables
    // cache eviction. GPU allocations still need memory permits.
    auto cache = std::make_shared<bioseq::indexing::gpu_minimizer_index_cache>(0u);
    const std::uint64_t operation_identity =
        bioseq::indexing::next_gpu_source_direct_operation_identity();
    if (operation_identity == 0u) {
        fail(ERRCODE_INTERNAL_ERROR, "MPF source-direct operation identity is zero");
        return;
    }
    prepared_target.execution_plan = std::move(execution_plan);
    prepared_target.target_rowno = static_cast<std::int64_t>(sequence_rowno);
    prepared_target.target_bp_size = target_bp_size;
    prepared_target.query_count = query_count;
    prepared_target.persisted_fragment_mapping_guard.emplace(std::move(mapping_guard));
    prepared_target.persisted_fragment_reader.emplace(std::move(*reader));
    prepared_target.resident_cache = std::move(cache);
    prepared_target.persisted_read_stats = read_stats;
    prepared_target.persisted_generation_id = head.generation_id;
    prepared_target.persisted_source_identity = head.source_identity;
    prepared_target.operation_identity = operation_identity;
    prepared_target.persisted = true;
    if (!prepared_target.source_ready()) {
        fail(ERRCODE_DATA_CORRUPTED,
             "MPF prepared source failed its final statement ownership check");
    }
}

} // namespace bioseq::simd
