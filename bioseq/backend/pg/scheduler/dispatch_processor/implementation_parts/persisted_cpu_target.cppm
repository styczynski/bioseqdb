/**
 * @file implementation_parts/persisted_cpu_target.cppm
 * @brief Owns authenticated persisted CPU target preparation and reader retention.
 */
#include <algorithm>
#include <array>
#include <bioseq/pg_build_profile.h>
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

export module bioseq.postgres.scheduler.dispatch_processor:persisted_cpu_target;

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

void batch_processor::prepare_cpu_persisted_native_target(
    const Oid database_oid,
    const Oid tablespace_oid,
    const Oid main_relfilenode,
    const Oid user_relid,
    const std::int16_t column_attno,
    const std::string_view index_name,
    const std::uint64_t sequence_rowno,
    const std::uint64_t target_bp_size,
    const std::uint8_t alphabet_code,
    const std::uint8_t storage_unit_byte_width,
    const std::span<const bioseq::pg_types::seq* const> query_headers,
    const std::span<const Datum> query_recipe_datums,
    const std::span<const pg_sequence_descriptor> query_recipe_descriptors,
    pg_sequence_read_session* query_read_session,
    const bioseq::storage::sequence_payload_allocation_observer* query_payload_observer,
    const std::span<const std::uint64_t> query_ordinals,
    const minimap2_parameters& base_params,
    const bioseq::storage::indexing::borrowed_executor_read_context& executor_read,
    const cpu_preparation_typmod_operations& typmod_ops,
    cpu_prepared_target& prepared_target) {
    if (!typmod_ops.matches(alphabet_code, storage_unit_byte_width)) {
        throw std::logic_error{
            "persisted CPU preparation typmod operations disagree with the request"};
    }
    using bioseq::indexing::minimizer_generation_encoding_error;
    using bioseq::indexing::minimizer_generation_layout;
    using bioseq::storage::indexing::authorize_minimizer_generation_catalog_source_pg;
    using bioseq::storage::indexing::format_seqidx_name;
    using bioseq::storage::indexing::index_storage_fragment_generation_read_session;
    using bioseq::storage::indexing::index_storage_hash_major_minimizer_read_session;
    using bioseq::storage::indexing::minimizer_fragment_image_mapped_open_validation;
    using bioseq::storage::indexing::minimizer_generation_administration_identity;
    using bioseq::storage::indexing::minimizer_generation_catalog_source_stats;
    using bioseq::storage::indexing::minimizer_generation_head;
    using bioseq::storage::indexing::minimizer_generation_manifest;
    using bioseq::storage::indexing::minimizer_generation_read_metadata;
    using bioseq::storage::indexing::minimizer_generation_read_stats;
    using bioseq::storage::indexing::minimizer_generation_relation_kind;
    using bioseq::storage::indexing::minimizer_generation_schema_error_name;
    using bioseq::storage::indexing::minimizer_generation_source_locator;
    using bioseq::storage::indexing::open_exact_hash_major_minimizer_read_session_pg;
    using bioseq::storage::indexing::open_exact_minimizer_fragment_image_pg;
    using bioseq::storage::indexing::open_exact_minimizer_generation_read_session_pg;
    const auto fail = [](std::string message) -> void {
        bioseq::postgres::support::invoke_pg_as_cpp([&message] {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("required compatible persisted minimizer index is invalid"),
                     errdetail("%s", message.c_str()),
                     errhint("Rebuild the exact grouped-SoA READY generation; "
                             "the required route has no recompute or cache fallback.")));
        });
    };
    const auto unavailable = [](std::string message) -> void {
        bioseq::postgres::support::invoke_pg_as_cpp([&message] {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("required compatible persisted minimizer index unavailable"),
                     errdetail("%s", message.c_str()),
                     errhint("Build the exact grouped-SoA READY generation; "
                             "the required route has no recompute or cache fallback.")));
        });
    };
    if (prepared_target.unions_ready || prepared_target.target_rowno < 0 ||
        static_cast<std::uint64_t>(prepared_target.target_rowno) != sequence_rowno ||
        !prepared_target.groups.empty() || prepared_target.query_union_permit.has_value() ||
        prepared_target.query_union_admission_bytes != 0u ||
        prepared_target.query_sketch_cache_permit.has_value() ||
        prepared_target.query_sketch_cache_admission_bytes != 0u || target_bp_size == 0u ||
        query_headers.empty() || query_recipe_datums.size() != query_headers.size() ||
        query_recipe_descriptors.size() != query_headers.size() ||
        query_ordinals.size() != query_headers.size() || database_oid != MyDatabaseId ||
        !OidIsValid(user_relid) || !OidIsValid(tablespace_oid) || !OidIsValid(main_relfilenode) ||
        column_attno <= 0 ||
        !executor_read.matches(database_oid, tablespace_oid, main_relfilenode, user_relid)) {
        fail("CPU native persisted prepared-target identity/lifecycle is invalid");
        return;
    }
    const minimizer_sketch_backend sketch_backend =
        pipe::target_index_sketch_backend_from_params(base_params);

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
    const std::uint64_t source_content_checksum =
        source_authorization.source_content_checksum_for_diagnostics();
    if (source.sequence_rowno != sequence_rowno ||
        source.locator.logical_length != target_bp_size || source.alphabet_code != alphabet_code ||
        source.storage_unit_byte_width != storage_unit_byte_width) {
        fail("authoritative source identity disagrees with the prepared target");
        return;
    }
    identity.table_name = format_seqidx_name(index_name,
                                             database_oid,
                                             tablespace_oid,
                                             main_relfilenode,
                                             column_attno,
                                             source.locator.bucket_index,
                                             source.locator.shard_index);

    auto opened_hash_major = open_exact_hash_major_minimizer_read_session_pg(
        identity.table_name, index_name, sequence_rowno, executor_read);
    if (!opened_hash_major.session && !opened_hash_major.head_absent) {
        switch (opened_hash_major.relation_failure.kind) {
        case minimizer_generation_relation_kind::missing:
            unavailable(fmt::format("generation relation {} is absent", identity.table_name));
            return;
        case minimizer_generation_relation_kind::malformed:
            fail(fmt::format(
                "generation relation {} does not match the current schema; schema_error={}",
                identity.table_name,
                minimizer_generation_schema_error_name(
                    opened_hash_major.relation_failure.schema_error)));
            return;
        case minimizer_generation_relation_kind::current_exact:
            fail("hash-major relation binder returned an invalid success classification");
            return;
        }
    }
    if (opened_hash_major.session && opened_hash_major.head_absent) {
        fail("hash-major binder returned both a session and an absence result");
        return;
    }
    bool hash_major_profile_applicable = false;
    if (opened_hash_major.session) {
        index_storage_hash_major_minimizer_read_session& hash_major_session =
            *opened_hash_major.session;
        const auto scheme = bioseq::indexing::minimizer_scheme_identity_for_backend(sketch_backend);
        const auto producer =
            bioseq::indexing::minimizer_producer_backend_identity_for_backend(sketch_backend);
        const auto hash_major_candidate_manifest = hash_major_session.manifest();
        const auto storage_profile = hash_major_candidate_manifest.storage_profile;
        if (!bioseq::indexing::is_valid_hash_major_minimizer_storage_profile(
                storage_profile, hash_major_candidate_manifest.target_length)) {
            fail("hash-major READY head names an invalid storage profile");
            return;
        }
        hash_major_profile_applicable =
            storage_profile.hash_scheme_identity == scheme.id &&
            storage_profile.hash_scheme_revision == scheme.revision &&
            storage_profile.alphabet_code == alphabet_code && storage_profile.kmer_size == 15u &&
            storage_profile.window_size == 10u &&
            storage_profile.hash_width_bytes == sizeof(std::uint32_t) &&
            storage_profile.producer_backend_identity == static_cast<std::uint16_t>(producer.id) &&
            storage_profile.producer_backend_revision == producer.revision;
        if (!hash_major_profile_applicable) {
            /* A self-authenticated but route-inapplicable hash-major profile is
             * the only present-head fallback case. No source authorization has been
             * consumed and no directory/control/posting callback has run, so
             * the independent READY grouped-SoA sentinel may still be opened. */
            hash_major_session.close_metadata_read();
            opened_hash_major.session.reset();
        } else {
            const bioseq::indexing::expected_hash_major_minimizer_generation hash_major_expected =
                hash_major_session.consume_source_authorization(std::move(source_authorization));
            if (hash_major_expected.generation_id == 0u ||
                hash_major_expected.query_key != sequence_rowno ||
                hash_major_expected.source_epoch != source.locator.source_epoch ||
                hash_major_expected.source_content_crc64 != source_content_checksum ||
                hash_major_expected.target_length != target_bp_size ||
                hash_major_expected.storage_profile != storage_profile) {
                fail("hash-major authenticated source binding disagrees with the active CPU route");
                return;
            }
            prepare_cpu_shared_query_unions(alphabet_code,
                                            storage_unit_byte_width,
                                            target_bp_size,
                                            query_headers,
                                            query_recipe_datums,
                                            query_recipe_descriptors,
                                            query_read_session,
                                            query_payload_observer,
                                            query_ordinals,
                                            base_params,
                                            /*include_persisted_parameter_group=*/true,
                                            cpu_query_union_storage::canonical_u32,
                                            typmod_ops,
                                            prepared_target);
            if (!prepared_target.unions_ready || prepared_target.groups.size() != 1u) {
                fail("hash-major canonical query preparation did not yield one group");
                return;
            }
            cpu_prepared_index_group& hash_major_group = prepared_target.groups.front();
            if (hash_major_group.kmer_size != 15 || hash_major_group.window_size != 10 ||
                hash_major_group.max_occ < 0 || hash_major_group.sketch_backend != sketch_backend ||
                !hash_major_group.query_hash_union.empty() ||
                hash_major_group.query_hash_union.capacity() != 0u ||
                hash_major_group.persisted_fragment_mapping_guard ||
                hash_major_group.persisted_fragment_reader ||
                hash_major_group.persisted_grouped_soa_reader ||
                hash_major_group.persisted_hash_major_reader || hash_major_group.target_index) {
                fail("hash-major canonical query group violates exclusive ownership");
                return;
            }
            const bioseq::indexing::canonical_query_hash_view canonical_view{
                .values32 =
                    std::span<const std::uint32_t>{hash_major_group.canonical_query_hash_union_u32},
                .width_bytes = sizeof(std::uint32_t),
                .storage_width_bytes = sizeof(std::uint32_t),
                .storage_profile_digest =
                    bioseq::indexing::hash_major_minimizer_storage_profile_digest(storage_profile),
            };
            const std::uint64_t hash_major_load_started_ns =
                static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
            auto hash_major_reader = bioseq::indexing::hash_major_minimizer_selective_reader::open(
                bioseq::parallelism::memory_admission_controller::instance(),
                hash_major_session.encoded_head(),
                hash_major_session.encoded_manifest(),
                hash_major_expected,
                hash_major_session.source_ref(),
                canonical_view,
                static_cast<std::uint64_t>(hash_major_group.max_occ));
            if (!hash_major_reader) {
                fail(fmt::format(
                    "hash-major selective reader rejected authenticated storage error={}",
                    static_cast<unsigned>(hash_major_reader.error())));
                return;
            }
            const auto hash_major_head = hash_major_session.head();
            const auto hash_major_manifest = hash_major_session.manifest();
            minimizer_generation_read_stats read_stats = hash_major_session.finish();
            const std::uint64_t hash_major_load_finished_ns =
                static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
            const std::uint64_t hash_major_load_ns =
                hash_major_load_finished_ns >= hash_major_load_started_ns
                    ? std::max<std::uint64_t>(
                          1u, hash_major_load_finished_ns - hash_major_load_started_ns)
                    : 1u;
            bioseq_log<logging_level::debug>(
                "[bioseq] hash-major selective reader: load_ns={} "
                "rows={} payload_bytes={} retained_positions={} "
                "stored_position_width={}",
                static_cast<unsigned long long>(hash_major_load_ns),
                static_cast<unsigned long long>(read_stats.hash_major.directory_rows +
                                                read_stats.hash_major.control_rows +
                                                read_stats.hash_major.posting_rows),
                static_cast<unsigned long long>(read_stats.hash_major.directory_bytes +
                                                read_stats.hash_major.control_bytes +
                                                read_stats.hash_major.posting_bytes),
                static_cast<unsigned long long>(
                    hash_major_reader->memory_stats().retained_position_count),
                static_cast<unsigned>(
                    hash_major_manifest.storage_profile.stored_posting_width_bytes));
            const auto& memory_stats = hash_major_reader->memory_stats();
            const std::uint64_t peak_admitted_bytes = std::max({
                memory_stats.directory_stage_peak_admitted_bytes,
                memory_stats.control_stage_peak_admitted_bytes,
                memory_stats.posting_stage_peak_admitted_bytes,
            });
            const std::uint64_t admitted_bytes = hash_major_reader->admitted_bytes();
            const std::uint64_t authorized_posting_bytes = bioseq::common::saturating_mul(
                memory_stats.retained_position_count,
                hash_major_manifest.storage_profile.stored_posting_width_bytes);
            if (hash_major_reader->target_length() != target_bp_size ||
                hash_major_head.generation_id != hash_major_expected.generation_id ||
                hash_major_head.source_identity != hash_major_manifest.source_identity ||
                hash_major_manifest.storage_profile != storage_profile ||
                read_stats.relation_opens != 1u || read_stats.head_scans != 1u ||
                read_stats.source_parent_rows_read != 1u ||
                read_stats.source_catalog_rows_read != 1u ||
                read_stats.source_catalog_index_scans != 1u ||
                read_stats.source_fragment_rows_read != 0u || read_stats.source_decoded_bp != 0u ||
                read_stats.hash_major.layout_revision !=
                    bioseq::indexing::k_hash_major_minimizer_layout_revision ||
                read_stats.hash_major.sessions != 1u ||
                read_stats.hash_major.generation_id != hash_major_head.generation_id ||
                read_stats.hash_major.source_identity != hash_major_head.source_identity ||
                read_stats.hash_major.source_content_crc64 !=
                    hash_major_head.source_content_crc64 ||
                read_stats.hash_major.storage_contract_digest !=
                    hash_major_head.storage_profile_digest ||
                memory_stats.directory_stream_crc_receipts != 1u ||
                memory_stats.control_block_crc_receipts != memory_stats.selected_block_count ||
                memory_stats.control_semantic_receipts != memory_stats.selected_block_count ||
                memory_stats.authorized_posting_bytes != authorized_posting_bytes ||
                memory_stats.generic_source_fallback_calls != 0u ||
                memory_stats.peak_admitted_bytes != peak_admitted_bytes ||
                memory_stats.retained_owner_bytes != admitted_bytes ||
                read_stats.hash_major.directory_row_crc_receipts !=
                    read_stats.hash_major.directory_rows ||
                read_stats.hash_major.control_row_crc_receipts !=
                    read_stats.hash_major.control_rows ||
                read_stats.hash_major.posting_row_crc_receipts !=
                    read_stats.hash_major.posting_rows ||
                read_stats.hash_major.posting_ranges_delivered !=
                    memory_stats.posting_ranges_delivered ||
                read_stats.hash_major.authorized_posting_bytes !=
                    memory_stats.authorized_posting_bytes ||
                memory_stats.posting_semantic_receipts != memory_stats.posting_ranges_delivered ||
                memory_stats.posting_range_crc_receipts > memory_stats.posting_ranges_delivered ||
                !source_stats.timed_equation_holds()) {
                fail("hash-major READY identity/profile/read-work validation disagrees");
                return;
            }
            auto& hash_major_stats = read_stats.hash_major;
            hash_major_stats.layout_revision =
                bioseq::indexing::k_hash_major_minimizer_layout_revision;
            hash_major_stats.sessions = 1u;
            hash_major_stats.generation_id = hash_major_head.generation_id;
            hash_major_stats.source_identity = hash_major_head.source_identity;
            hash_major_stats.source_content_crc64 = hash_major_head.source_content_crc64;
            hash_major_stats.storage_contract_digest = hash_major_head.storage_profile_digest;
            hash_major_stats.directory_stream_crc_receipts =
                memory_stats.directory_stream_crc_receipts;
            hash_major_stats.control_block_crc_receipts = memory_stats.control_block_crc_receipts;
            hash_major_stats.control_semantic_receipts = memory_stats.control_semantic_receipts;
            hash_major_stats.posting_range_crc_receipts = memory_stats.posting_range_crc_receipts;
            hash_major_stats.posting_semantic_receipts = memory_stats.posting_semantic_receipts;
            hash_major_stats.query_count = memory_stats.query_count;
            hash_major_stats.selected_block_count = memory_stats.selected_block_count;
            hash_major_stats.accepted_hash_count = memory_stats.accepted_hash_count;
            hash_major_stats.retained_position_count = memory_stats.retained_position_count;
            hash_major_stats.directory_stage_peak_bytes =
                memory_stats.directory_stage_peak_admitted_bytes;
            hash_major_stats.control_stage_peak_bytes =
                memory_stats.control_stage_peak_admitted_bytes;
            hash_major_stats.posting_stage_peak_bytes =
                memory_stats.posting_stage_peak_admitted_bytes;
            hash_major_stats.peak_admitted_bytes = memory_stats.peak_admitted_bytes;
            hash_major_stats.retained_owner_bytes = memory_stats.retained_owner_bytes;
            hash_major_stats.admitted_bytes = admitted_bytes;
            hash_major_stats.generic_source_fallback_calls =
                memory_stats.generic_source_fallback_calls;
            hash_major_stats.failures = 0u;
            hash_major_stats.saturated = false;
            hash_major_group.persisted_hash_major_reader.emplace(std::move(*hash_major_reader));
            hash_major_group.target_index_source =
                ::bioseq::pipeline_stats::target_index_source::persisted;
            hash_major_group.persisted_generation_id = hash_major_head.generation_id;
            hash_major_group.persisted_hash_major_source_identity = hash_major_head.source_identity;
            hash_major_group.persisted_hash_major_source_identity_present = true;
            /* The diagnostic u64 pipeline correlation value is deliberately not used
             * to authenticate hash-major; correctness above is bound to all 128
             * source-identity bits. */
            hash_major_group.persisted_source_identity =
                hash_major_source_diagnostic_token(hash_major_head.source_identity);
            hash_major_group.persisted_read_stats = read_stats;
            hash_major_group.persisted_reader_loads = 1u;
            hash_major_group.build_set_checksum_present = true;
            hash_major_group.build_set_checksum = hash_major_group.key_set_checksum;
            ++prepared_target.physical_target_index_reads;
            return;
        }
    }
    /* The binder's head_absent result is an authenticated absence of the
     * distinct hash-major sentinel.  Together with the inapplicable-profile case
     * above, this is before every hash-major stream payload callback and therefore the
     * only clean boundary at which grouped-SoA may be tried. */
    if (!hash_major_grouped_soa_fallback_allowed(opened_hash_major.head_absent,
                                                 hash_major_profile_applicable,
                                                 /*stream_io_started=*/false)) {
        fail("hash-major dispatch crossed the fail-closed fallback boundary");
        return;
    }
    opened_hash_major.session.reset();
    const auto expected = bioseq::indexing::expected_grouped_soa_generation_identity_for_backend(
        minimizer_generation_layout::grouped_soa_segment_local,
        sketch_backend,
        source.alphabet_code,
        source.storage_unit_byte_width,
        15u,
        10u,
        bioseq::indexing::k_grouped_soa_default_max_occurrence_cap);
    if (!expected) {
        fail(fmt::format("grouped-SoA storage contract rejected storage_error={}",
                         static_cast<unsigned>(expected.error())));
        return;
    }
    const auto configured_minimizer_fragment_grouped_soa_lookup_route =
        bioseq::aligner::settings::fragment_minimizer::lookup_route.get_value();
    const bool compact_h32_direct_configured =
        configured_minimizer_fragment_grouped_soa_lookup_route ==
        bioseq::aligner::fragment_minimizer_lookup_route::direct_compact_hash_lookup;
    const bool compact_h32_direct_requested =
        compact_h32_direct_configured &&
        index_name == bioseq::indexing::k_minimizers_per_fragment_index_name;
    auto opened = open_exact_minimizer_generation_read_session_pg(
        identity.table_name, index_name, sequence_rowno, executor_read, *expected);
    if (!opened) {
        switch (opened.failure.kind) {
        case minimizer_generation_relation_kind::missing:
            unavailable(fmt::format("generation relation {} is absent", identity.table_name));
            return;
        case minimizer_generation_relation_kind::malformed:
            fail(fmt::format(
                "generation relation {} does not match the current schema; schema_error={}",
                identity.table_name,
                minimizer_generation_schema_error_name(opened.failure.schema_error)));
            return;
        case minimizer_generation_relation_kind::current_exact:
            fail("generation relation binder returned an invalid success classification");
            return;
        }
    }
    index_storage_fragment_generation_read_session& session = *opened.session;
    const auto head = session.read_active_head_once();
    std::optional<std::span<const std::byte>> authenticated_grouped_soa_directory{};
    const minimizer_generation_read_metadata* metadata = nullptr;
    minimizer_generation_manifest manifest{};
    if (compact_h32_direct_requested) {
        manifest = session.read_manifest_once_without_directory(head);
    } else {
        metadata = &session.read_manifest_directory_once(head);
        manifest = metadata->manifest;
        authenticated_grouped_soa_directory = metadata->directory.encoded_bytes();
    }
    if (manifest.layout != minimizer_generation_layout::grouped_soa_segment_local) {
        fail("READY generation is not the current grouped-SoA layout");
        return;
    }
    const bool mapped_minimizer_fragment_grouped_soa =
        index_name == bioseq::indexing::k_minimizers_per_fragment_index_name;
    const bool full_minimizer_sequence_grouped_soa =
        index_name == bioseq::indexing::k_minimizers_per_sequence_index_name;
    if (!mapped_minimizer_fragment_grouped_soa && !full_minimizer_sequence_grouped_soa) {
        fail("grouped-SoA READY generation is bound to an unsupported logical index");
        return;
    }
    if (compact_h32_direct_requested && !mapped_minimizer_fragment_grouped_soa) {
        fail(
            "compact-H32 direct lookup requires a minimizer-fragment grouped-SoA READY generation");
        return;
    }
    const auto& selected_expected = *expected;
    session.consume_source_authorization(std::move(source_authorization));
    auto& query_prep_pool = bioseq::parallelism::global_pool::instance();
    /* This is the exact eligibility boundary of the typed full-batch native
     * route. Outside it, preserve the synchronous canonical-u32 validation because
     * no later complete query_hash_plan stream is guaranteed to exist. */
    const bool defer_compact_direct_query_set_validation =
        compact_h32_direct_requested && base_params.max_occ > 0 && !base_params.enable_profiling &&
        target_bp_size > std::uint64_t{256000000} &&
        std::ranges::all_of(query_recipe_datums,
                            [](Datum datum) noexcept { return datum == Datum{0}; }) &&
        query_headers.size() >= bioseq::parallelism::task_parallel_minimums::k_query_count &&
        bioseq::parallelism::has_parallel_tasks(query_prep_pool.get_mode()) &&
        query_prep_pool.maximum_concurrency() > 1u;
    prepare_cpu_shared_query_unions(alphabet_code,
                                    storage_unit_byte_width,
                                    target_bp_size,
                                    query_headers,
                                    query_recipe_datums,
                                    query_recipe_descriptors,
                                    query_read_session,
                                    query_payload_observer,
                                    query_ordinals,
                                    base_params,
                                    /*include_persisted_parameter_group=*/true,
                                    mapped_minimizer_fragment_grouped_soa
                                        ? (defer_compact_direct_query_set_validation
                                               ? cpu_query_union_storage::retained_sketch_only
                                               : cpu_query_union_storage::canonical_u32)
                                        : cpu_query_union_storage::minimizer_sketch,
                                    typmod_ops,
                                    prepared_target);
    if (!prepared_target.unions_ready || prepared_target.groups.size() != 1u) {
        fail("grouped-SoA query preparation did not yield one group");
        return;
    }
    cpu_prepared_index_group& group = prepared_target.groups.front();
    if (group.kmer_size != 15 || group.window_size != 10 || group.max_occ < 0 ||
        group.sketch_backend != sketch_backend ||
        (group.compact_direct_query_set_validation_pending &&
         (!defer_compact_direct_query_set_validation || group.max_occ <= 0 ||
          !group.canonical_query_hash_union_u32.empty() ||
          group.canonical_query_hash_union_u32.capacity() != 0u)) ||
        (mapped_minimizer_fragment_grouped_soa
             ? (!group.query_hash_union.empty() || group.query_hash_union.capacity() != 0u)
             : (!group.canonical_query_hash_union_u32.empty() ||
                group.canonical_query_hash_union_u32.capacity() != 0u)) ||
        group.persisted_fragment_mapping_guard || group.persisted_fragment_reader ||
        group.persisted_grouped_soa_reader || group.persisted_hash_major_reader ||
        group.target_index) {
        fail("grouped-SoA parameter group is not the pinned exclusive cold-read shape");
        return;
    }
    // Minimizer-fragment storage remains a collection of independent physical-fragment
    // indexes. The mmap image is the byte-exact artifact produced by the
    // writer's complete authenticated READY reread, fsync, and durable rename.
    // Cold open checks the exact generation identity and prepares only fragment
    // address plans. Query-wave max-occ reduction is fused into the parallel
    // fragment probe; no target-wide posting/count owner is constructed.
    if (mapped_minimizer_fragment_grouped_soa) {
        using fragment_reader_type = bioseq::indexing::prepared_minimizer_fragment_reader;
        const std::uint64_t fragment_load_started_ns =
            static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        const auto storage_descriptor = session.read_live_storage_descriptor_once(identity);
        auto mapped_image = open_exact_minimizer_fragment_image_pg(
            identity,
            sequence_rowno,
            head,
            manifest,
            storage_descriptor,
            compact_h32_direct_requested
                ? minimizer_fragment_image_mapped_open_validation::bounded_header_only
                : minimizer_fragment_image_mapped_open_validation::full_metadata);
        std::optional<fragment_reader_type> fragment_reader{};
        minimizer_generation_read_stats read_stats{};
        if (compact_h32_direct_requested) {
            const auto direct_profile =
                bioseq::indexing::derive_grouped_soa_minimizer_profile(manifest);
            if (!direct_profile || direct_profile->max_hash == 0u ||
                direct_profile->max_hash > (std::numeric_limits<std::uint32_t>::max)() ||
                direct_profile->max_occ_cap == 0u ||
                direct_profile->max_occ_cap > (std::numeric_limits<std::uint32_t>::max)()) {
                fail("compact-H32 direct profile geometry is invalid");
                return;
            }
            const std::uint32_t profile_max_hash =
                static_cast<std::uint32_t>(direct_profile->max_hash);
            const std::uint32_t profile_hash_bits = std::bit_width(profile_max_hash);
            const std::uint64_t exact_profile_max_hash =
                (std::uint64_t{1u} << profile_hash_bits) - 1u;
            const std::span<const std::byte> direct_section = mapped_image.compact_h32_direct();
            const auto direct_header_receipt = mapped_image.compact_h32_direct_header_receipt();
            if (profile_hash_bits == 0u ||
                profile_hash_bits > (std::numeric_limits<std::uint8_t>::max)() ||
                exact_profile_max_hash != direct_profile->max_hash || direct_section.empty() ||
                !direct_header_receipt) {
                fail("compact-H32 direct mapped section is absent or incompatible");
                return;
            }
            auto mapped_owner = mapped_image.borrow_mapping_token();
            const bioseq::indexing::compact_hash32_bounded_open_options direct_open_options{
                .expected_identity =
                    bioseq::indexing::direct_index_identity{
                        .generation_id = head.generation_id,
                        .source_identity = head.source_identity,
                        .storage_contract_checksum = head.storage_contract_checksum,
                        .target_length = manifest.target_length,
                    },
                .expected_max_occ_cap = static_cast<std::uint32_t>(direct_profile->max_occ_cap),
                .expected_geometry =
                    bioseq::indexing::compact_hash32_geometry{
                        .hash_domain_bits = static_cast<std::uint8_t>(profile_hash_bits),
                        .max_hash = profile_max_hash,
                    },
                .expected_direct_header_crc64 = direct_header_receipt,
            };
            auto bounded_direct = bioseq::indexing::open_compact_hash32_direct_index_bounded(
                std::move(mapped_owner), direct_section, direct_open_options);
            if (!bounded_direct ||
                bounded_direct->stats().direct_section_bytes != direct_section.size() ||
                bounded_direct->stats().header_bytes_examined !=
                    bioseq::indexing::k_compact_hash32_direct_index_header_bytes ||
                bounded_direct->stats().payload_bytes_examined != 0u ||
                !bounded_direct->stats().expected_direct_header_crc64_checked) {
                fail(fmt::format("compact-H32 bounded open rejected direct_error={}",
                                 bounded_direct ? 0u
                                                : static_cast<unsigned>(bounded_direct.error())));
                return;
            }
            // The nested fixed header and its outer CRC receipt are now
            // authenticated. Start fail-open metadata readahead now; selected
            // posting pages are advised by the existing descriptor-layout pass.
            // The unrelated outer minimizer-fragment payload remains demand-paged.
            mapped_image.advise_compact_h32_direct_metadata_readahead(bounded_direct->view());
            const auto selected_page_adviser =
                mapped_image.borrow_compact_h32_selected_page_adviser(bounded_direct->view());
            auto owned_direct = bioseq::indexing::owned_compact_hash32_direct_index_view::bind(
                bounded_direct->image_owner(), bounded_direct->view());
            if (!owned_direct) {
                fail(fmt::format("compact-H32 owner binding rejected wire_error={}",
                                 static_cast<unsigned>(owned_direct.error())));
                return;
            }
            auto made = fragment_reader_type::open_compact_direct(
                bioseq::parallelism::memory_admission_controller::instance(),
                head,
                manifest,
                selected_expected,
                static_cast<std::uint64_t>(group.max_occ),
                std::move(*owned_direct),
                selected_page_adviser);
            if (!made) {
                fail(fmt::format("compact-H32 fragment reader rejected wire_error={}",
                                 static_cast<unsigned>(made.error())));
                return;
            }
            fragment_reader.emplace(std::move(*made));
            read_stats = session.finish_mapped_compact_direct();
        } else {
            if (!authenticated_grouped_soa_directory) {
                fail("grouped-SoA control-scan route has no authenticated directory");
                return;
            }
            const std::span<const std::byte> mapped_payload = mapped_image.payload();
            auto mapped_payload_owner = mapped_image.borrow_mapping_token();
            auto made = fragment_reader_type::open(
                bioseq::parallelism::memory_admission_controller::instance(),
                head,
                manifest,
                selected_expected,
                *authenticated_grouped_soa_directory,
                static_cast<std::uint64_t>(group.max_occ),
                std::move(mapped_payload_owner),
                mapped_payload);
            if (!made) {
                fail(fmt::format("fragment-native grouped-SoA reader rejected wire_error={}",
                                 static_cast<unsigned>(made.error())));
                return;
            }
            fragment_reader.emplace(std::move(*made));
            read_stats = session.finish_mapped_minimizer_fragment_payload();
        }
        const std::uint64_t fragment_load_finished_ns =
            static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        group.persisted_reader_load_host_wall_ns =
            fragment_load_finished_ns >= fragment_load_started_ns
                ? std::max<std::uint64_t>(1u, fragment_load_finished_ns - fragment_load_started_ns)
                : 1u;
        const auto scheme =
            bioseq::indexing::minimizer_scheme_identity_for_backend(group.sketch_backend);
        const auto producer_backend =
            bioseq::indexing::minimizer_producer_backend_identity_for_backend(group.sketch_backend);
        const std::uint64_t expected_metadata_rows = 2u + read_stats.directory_chunks_read;
        const std::uint64_t expected_metadata_bytes =
            minimizer_generation_head::encoded_bytes +
            bioseq::indexing::minimizer_manifest_encoded_bytes(manifest.layout) +
            read_stats.directory_bytes_read;
        const bool route_read_validation_matches =
            compact_h32_direct_requested
                ? fragment_reader->compact_direct_ready() &&
                      fragment_reader->compact_direct_only() &&
                      fragment_reader->mapped_grouped_soa_payload_bytes() == 0u &&
                      fragment_reader->grouped_soa_directory_entry_count() == 0u &&
                      read_stats.directory_chunks_read == 0u &&
                      read_stats.directory_bytes_read == 0u
                : !fragment_reader->compact_direct_only() &&
                      fragment_reader->mapped_grouped_soa_payload_bytes() ==
                          manifest.payload_bytes &&
                      read_stats.directory_chunks_read == manifest.directory_chunk_count &&
                      read_stats.directory_bytes_read == manifest.directory_bytes;
        const bool read_validation_matches =
            fragment_reader->ready() && route_read_validation_matches &&
            fragment_reader->target_length() == prepared_target.target_bp_size &&
            manifest.kmer_size == group.kmer_size && manifest.window_size == group.window_size &&
            manifest.minimizer_scheme_id == scheme.id &&
            manifest.minimizer_scheme_revision == scheme.revision &&
            manifest.producer_backend_id == producer_backend.id &&
            manifest.producer_identity_revision == producer_backend.revision &&
            manifest.max_occ_cap == bioseq::indexing::k_grouped_soa_default_max_occurrence_cap &&
            head.generation_id != 0 && head.source_identity != 0 &&
            read_stats.relation_opens == 1 && read_stats.head_scans == 1 &&
            read_stats.head_rows_read == 1 && read_stats.manifest_directory_scans == 1 &&
            read_stats.manifest_rows_read == 1 &&
            read_stats.metadata_heap_rows_read == expected_metadata_rows &&
            read_stats.metadata_bytes_read == expected_metadata_bytes &&
            read_stats.total_heap_rows_read == expected_metadata_rows &&
            read_stats.total_bytes_read == expected_metadata_bytes &&
            read_stats.explicit_copy_bytes == expected_metadata_bytes &&
            read_stats.toast_values_read == 0 && read_stats.detoast_bytes == 0 &&
            read_stats.metadata_repeated_reads == 0 && read_stats.full_generation.scan_calls == 0 &&
            read_stats.bulk_read_calls == 0 && read_stats.range_scan_calls == 0 &&
            read_stats.index_scans == 2 && read_stats.payload.delivered_chunks == 0 &&
            read_stats.payload.heap_rows_read == 0 && read_stats.payload.delivered_bytes == 0 &&
            read_stats.payload_bytes_read == 0 && read_stats.cold.route_empty == 0 &&
            read_stats.cold.route_selective == 0 && read_stats.cold.route_full == 0 &&
            read_stats.cold.head_scans == 1 && read_stats.cold.directory_scans == 1 &&
            read_stats.source_parent_rows_read == 1 && read_stats.source_catalog_rows_read == 1 &&
            read_stats.source_catalog_index_scans == 1 &&
            read_stats.source_fragment_rows_read == 0 && read_stats.source_decoded_bp == 0 &&
            source_stats.timed_equation_holds();
        if (!read_validation_matches) {
            fail("grouped-SoA READY identity/backend/scheme/read-work validation disagrees");
            return;
        }

        auto mapping_guard = std::move(mapped_image).release_mapping_lease_guard();
        if (!mapping_guard) {
            fail("grouped-SoA mapped reader lost its mmap/fd lifetime authority");
            return;
        }
        group.persisted_fragment_mapping_guard.emplace(std::move(mapping_guard));
        group.persisted_fragment_reader.emplace(std::move(*fragment_reader));
        group.persisted_fragment_lookup_route =
            compact_h32_direct_requested
                ? bioseq::indexing::minimizer_fragment_lookup_route::direct_compact_hash_lookup
                : bioseq::indexing::minimizer_fragment_lookup_route::grouped_soa_control_scans;
        group.target_index_source = ::bioseq::pipeline_stats::target_index_source::persisted;
        group.persisted_generation_id = head.generation_id;
        group.persisted_source_identity = head.source_identity;
        group.persisted_read_stats = read_stats;
        group.persisted_reader_loads = 1;
        if (!group.compact_direct_query_set_validation_pending) {
            group.build_set_checksum_present = true;
            group.build_set_checksum = group.key_set_checksum;
        } else if (!compact_h32_direct_requested || group.key_set_checksum != 0u ||
                   group.build_set_checksum_present || group.build_set_checksum != 0u) {
            fail("compact-H32 deferred query-set validation crossed reader publication");
            return;
        }
        ++prepared_target.physical_target_index_reads;
        return;
    }

    /* Minimizer-sequence grouped-SoA remains a sequence-wide persisted index. Unlike the
     * minimizer-fragment route above, it cannot open the minimizer-fragment mmap sidecar: read
     * and authenticate the complete PostgreSQL payload into the established
     * query-selected grouped-SoA reader. */
    if (full_minimizer_sequence_grouped_soa) {
        using grouped_soa_reader_type = bioseq::indexing::prepared_grouped_soa_query_reader;
        if (!authenticated_grouped_soa_directory) {
            fail("minimizer-sequence grouped-SoA route has no authenticated directory");
            return;
        }
        const std::uint64_t grouped_soa_load_started_ns =
            static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        auto made = grouped_soa_reader_type::begin(
            bioseq::parallelism::memory_admission_controller::instance(),
            head,
            manifest,
            selected_expected,
            *authenticated_grouped_soa_directory,
            group.query_hash_union,
            static_cast<std::uint64_t>(group.max_occ));
        if (!made) {
            fail(
                fmt::format("minimizer-sequence grouped-SoA reader begin rejected storage_error={}",
                            static_cast<unsigned>(made.error())));
            return;
        }
        std::optional<grouped_soa_reader_type> grouped_soa_reader{std::in_place, std::move(*made)};
        /* The reader owns the exact compact query-hash image from this point.
         * Release the scheduler copy before allocating retained payload state. */
        if (!prepared_target.release_group_query_hash_union_noexcept(group)) {
            fail("minimizer-sequence grouped-SoA canonical query-union release disagrees with "
                 "admission ownership");
            return;
        }

        std::optional<minimizer_generation_encoding_error> callback_error;
        std::exception_ptr callback_exception;
        const auto accept = [&callback_error, &callback_exception, &grouped_soa_reader](
                                const bioseq::indexing::validated_minimizer_generation_row_receipt&
                                    receipt) noexcept {
            if (callback_error || callback_exception)
                return;
            try {
                auto accepted = grouped_soa_reader->accept(receipt);
                if (!accepted)
                    callback_error = accepted.error();
            } catch (...) {
                callback_exception = std::current_exception();
            }
        };
        (void)session.read_full_generation_payload_once(accept, accept);
        if (callback_exception) {
            std::rethrow_exception(callback_exception);
        }
        if (callback_error) {
            fail(fmt::format(
                "minimizer-sequence grouped-SoA payload ingest rejected storage_error={}",
                static_cast<unsigned>(*callback_error)));
            return;
        }
        if (auto finished = grouped_soa_reader->finish(); !finished) {
            fail(fmt::format(
                "minimizer-sequence grouped-SoA reader final validation rejected storage_error={}",
                static_cast<unsigned>(finished.error())));
            return;
        }

        const minimizer_generation_read_stats read_stats = session.finish();
        const std::uint64_t grouped_soa_load_finished_ns =
            static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        group.persisted_reader_load_host_wall_ns =
            grouped_soa_load_finished_ns >= grouped_soa_load_started_ns
                ? std::max<std::uint64_t>(
                      1u, grouped_soa_load_finished_ns - grouped_soa_load_started_ns)
                : 1u;
        const auto scheme =
            bioseq::indexing::minimizer_scheme_identity_for_backend(group.sketch_backend);
        const auto producer_backend =
            bioseq::indexing::minimizer_producer_backend_identity_for_backend(group.sketch_backend);
        const bool read_validation_matches =
            grouped_soa_reader->ready() &&
            grouped_soa_reader->target_length() == prepared_target.target_bp_size &&
            manifest.kmer_size == group.kmer_size && manifest.window_size == group.window_size &&
            manifest.minimizer_scheme_id == scheme.id &&
            manifest.minimizer_scheme_revision == scheme.revision &&
            manifest.producer_backend_id == producer_backend.id &&
            manifest.producer_identity_revision == producer_backend.revision &&
            manifest.max_occ_cap == bioseq::indexing::k_grouped_soa_default_max_occurrence_cap &&
            head.generation_id != 0 && head.source_identity != 0 &&
            read_stats.relation_opens == 1 && read_stats.head_scans == 1 &&
            read_stats.manifest_directory_scans == 1 && read_stats.metadata_repeated_reads == 0 &&
            read_stats.full_generation.scan_calls == 1 && read_stats.bulk_read_calls == 1 &&
            read_stats.range_scan_calls == 1 && read_stats.index_scans == 3 &&
            read_stats.payload.delivered_chunks == read_stats.payload.heap_rows_read &&
            read_stats.payload.delivered_bytes == manifest.payload_bytes &&
            read_stats.cold.route_empty == 0 && read_stats.cold.route_selective == 0 &&
            read_stats.cold.route_full == 1 && read_stats.source_parent_rows_read == 1 &&
            read_stats.source_catalog_rows_read == 1 &&
            read_stats.source_catalog_index_scans == 1 &&
            read_stats.source_fragment_rows_read == 0 && read_stats.source_decoded_bp == 0 &&
            source_stats.timed_equation_holds();
        if (!read_validation_matches) {
            fail("minimizer-sequence grouped-SoA READY identity/backend/scheme/read-work "
                 "validation disagrees");
            return;
        }

        group.persisted_grouped_soa_reader.emplace(std::move(*grouped_soa_reader));
        group.target_index_source = ::bioseq::pipeline_stats::target_index_source::persisted;
        group.persisted_generation_id = head.generation_id;
        group.persisted_source_identity = head.source_identity;
        group.persisted_read_stats = read_stats;
        group.persisted_reader_loads = 1;
        group.build_set_checksum_present = true;
        group.build_set_checksum = group.key_set_checksum;
        ++prepared_target.physical_target_index_reads;
        return;
    }
}

} // namespace bioseq::simd
