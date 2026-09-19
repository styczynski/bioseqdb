/**
 * @file admin/grouped_soa.cppm
 * @brief Grouped-SoA minimizer generation encoding and publication.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/xact.h"
#include "miscadmin.h"
#include "utils/errcodes.h"
#include "utils/snapmgr.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
export module bioseq.postgres.storage.indexing.admin:grouped_soa;

import bioseq.common.logging;
import bioseq.postgres.storage.indexing.admin.diagnostic_support;
import bioseq.postgres.storage.indexing.admin.index_build_runtime;
import bioseq.common.seqan_adapter;
import bioseq.postgres.support;
import bioseq.storage.layout;
import bioseq.storage.lazy_sequence_view;
import bioseq.postgres.storage.scanner.lazy_binding_contract;
import bioseq.sequences;                       // compact_sequence_range
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.sequences.kmers; // minimizer/index APIs
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.index_relation_identity;
import :hash_major;

static_assert(__cplusplus >= 202302L, "C++23-or-later");

namespace bioseq::storage::indexing::detail {

/// Avoid the allocating PostgreSQL error bridge on the ordinary no-interrupt path.
/// A pending query-cancel ERROR crosses the bridge and unwinds statement-owned C++
/// state; backend-termination/FATAL retains PostgreSQL's native terminal semantics.
void check_generation_interrupts_as_cpp(void*) {
    if (!INTERRUPTS_PENDING_CONDITION()) {
        return;
    }
    bioseq::postgres::support::invoke_pg_as_cpp([] {
        CHECK_FOR_INTERRUPTS();
    });
}

} // namespace bioseq::storage::indexing::detail

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
namespace bioseq::storage::indexing::detail {

// Preserve existing producer counters before the ordinary update_index wrapper
// discards its validated publication. This is not a transaction-commit receipt.
void log_e2e_minimizer_publication_execution(
    const ready_minimizer_generation_publication& publication,
    Oid user_relid,
    std::int16_t column_attno,
    std::string_view index_name,
    std::uint64_t query_key) {
    const char* receipt_kind = "";
    const char* scan_counter = "";
    const char* retry_counter = "";
    std::uint64_t scans = 0, retries = 0, short_runs = 0;
    std::uint64_t scalar_runs = 0, exact_runs = 0, bounded_runs = 0, vulkan_runs = 0;
    std::uint64_t roundtrips = 0, downloaded = 0, window_splits = 0;
    bool receipt_saturated = false, vulkan_saturated = false;

    if (const auto* receipt =
            std::get_if<bioseq::indexing::minimizer_fragment_generation_execution_receipt>(
                &publication.execution())) {
        receipt_kind = "fragment";
        scan_counter = "producer_scan_count";
        retry_counter = "geometry_retry_count";
        scans = receipt->producer_scan_count;
        retries = receipt->geometry_retry_count;
        short_runs = receipt->short_core_runs;
        scalar_runs = receipt->scalar_core_runs;
        exact_runs = receipt->exact_simd_core_runs;
        bounded_runs = receipt->bounded_nthash_core_runs;
        vulkan_runs = receipt->vulkan_core_runs;
        roundtrips = receipt->vulkan_stats.completed_roundtrips;
        downloaded = receipt->vulkan_stats.downloaded_minimizers;
        window_splits = receipt->vulkan_stats.gpu_window_splits;
        receipt_saturated = receipt->saturated;
        vulkan_saturated = receipt->vulkan_stats.saturated;
    } else if (const auto* receipt =
                   std::get_if<bioseq::indexing::minimizer_sequence_generation_execution_receipt>(
                       &publication.execution())) {
        receipt_kind = "sequence";
        scan_counter = "replay_scan_count";
        retry_counter = "replay_reroute_pass_count";
        scans = receipt->replay_scan_count;
        retries = receipt->replay_reroute_pass_count;
        short_runs = receipt->short_core_runs;
        scalar_runs = receipt->scalar_core_runs;
        exact_runs = receipt->exact_simd_core_runs;
        bounded_runs = receipt->bounded_nthash_core_runs;
        vulkan_runs = receipt->vulkan_core_runs;
        roundtrips = receipt->vulkan_stats.completed_roundtrips;
        downloaded = receipt->vulkan_stats.downloaded_minimizers;
        window_splits = receipt->vulkan_stats.gpu_window_splits;
        receipt_saturated = receipt->saturated;
        vulkan_saturated = receipt->vulkan_stats.saturated;
    } else {
        return;
    }

    bioseq_log<logging_level::info, logging_target::postgres>(
        "[bioseq_e2e] event=validated_minimizer_publication_execution version=1 "
        "receipt_kind={} backend_pid={} database_oid={} user_relid={} column_attno={} "
        "index_name={} query_key={} generation_id={} source_identity={:016x} "
        "source_content_checksum={:016x} storage_contract_checksum={:016x} "
        "selected_backend={} {}={} {}={} short_core_runs={} scalar_core_runs={} "
        "exact_simd_core_runs={} bounded_nthash_core_runs={} vulkan_core_runs={} "
        "vulkan_completed_roundtrips={} vulkan_downloaded_minimizers={} "
        "vulkan_gpu_window_splits={} receipt_saturated={} vulkan_saturated={}",
        receipt_kind,
        MyProcPid,
        static_cast<unsigned>(MyDatabaseId),
        static_cast<unsigned>(user_relid),
        static_cast<int>(column_attno),
        index_name,
        query_key,
        publication.generation_id(),
        publication.source_identity(),
        publication.source_content_checksum(),
        publication.storage_contract_checksum(),
        bioseq::minimizers::minimizer_sketch_backend_name(publication.selected_backend()),
        scan_counter, scans, retry_counter, retries,
        short_runs, scalar_runs, exact_runs, bounded_runs, vulkan_runs,
        roundtrips, downloaded, window_splits, receipt_saturated, vulkan_saturated);
}

} // namespace bioseq::storage::indexing::detail
#endif

export namespace bioseq::storage::indexing {

/**
 * @brief Build, completely reread, and atomically publish one grouped-SoA generation.
 * @tparam p_sequence_range Compact source sequence range.
 * @param user_relid Source Bioseq relation.
 * @param column_attno Source sequence attribute.
 * @param index_name Registered fragment or sequence minimizer index.
 * @param sequence_rowno Source-row ordinal and query key.
 * @param storage PostgreSQL generation storage authority.
 * @param sequence Snapshot-bound lazy sequence view.
 * @param source_binding Locator and source-snapshot binding.
 * @param algorithm_identity Generated identity frozen before publication starts.
 * @param backend Exact producer backend.
 * @param kernels Required backend kernel table.
 * @return Published generation identity and execution receipt.
 *
 * The caller retains snapshot and metadata-age ownership. A canonical sidecar is created or
 * reused, and head publication remains impossible before the private complete-reread token.
 */
template <class p_sequence_range>
[[nodiscard]] inline ready_minimizer_generation_publication
build_and_publish_grouped_soa_minimizer_generation_pg(
    Oid user_relid,
    std::int16_t column_attno,
    std::string_view index_name,
    std::uint64_t sequence_rowno,
    fragment_index_storage& storage,
    bioseq::storage::lazy_sequence_view<
        typename bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>::alphabet_type>
        sequence,
    bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range> source_binding,
    const bioseq::indexing::minimizer_generation_algorithm_identity& algorithm_identity,
    bioseq::minimizers::minimizer_sketch_backend backend,
    const bioseq::minimizers::minimizer_kernels* kernels) {
    using source_binding_type = bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>;
    using backend_type = bioseq::minimizers::minimizer_sketch_backend;
    using fragment_index = bioseq::indexing::minimizers_per_fragment_index<fragment_index_storage>;
    using sequence_index = bioseq::indexing::direct_minimizer_bucket_index<fragment_index_storage>;
    require_selected_minimizer_kernels(backend, kernels, "grouped-SoA minimizer generation");
    using traits_type = typename compact_sequence_range<p_sequence_range>::traits;
    using canonical_sequence_config = sequence_custom_alphabet<typename traits_type::alphabet_type,
                                                               typename traits_type::storage_unit>;
    static_assert(traits_type::bits_per_char ==
                      compact_sequence_range<canonical_sequence_config>::traits::bits_per_char,
                  "READY minimizer coordinator requires canonical PostgreSQL packing");
    const sequence_locator& locator = source_binding.locator();
    Snapshot registered_source_snapshot = source_binding.registered_snapshot();

    const bool is_fragment_index = index_name == k_minimizers_per_fragment_index_name;
    const bool is_sequence_index = index_name == k_minimizers_per_sequence_index_name;
    const bool active_snapshot_ready = bioseq::postgres::support::invoke_pg_as_cpp(
        [] { return ActiveSnapshotSet() && IsMVCCSnapshot(GetActiveSnapshot()); });

    if (!OidIsValid(user_relid) || column_attno <= 0 ||
        source_binding.column_attno() != column_attno ||
        (!is_fragment_index && !is_sequence_index) ||
        !bioseq::indexing::sql_bigint_can_represent(sequence_rowno) ||
        locator.payload_version != sequence_locator_payload_version || locator.source_epoch == 0 ||
        locator.logical_length == 0 || locator.fragment_count == 0 ||
        registered_source_snapshot == nullptr || !IsMVCCSnapshot(registered_source_snapshot) ||
        registered_source_snapshot->regd_count == 0 || !active_snapshot_ready) {
        ready_minimizer_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE,
                                     "READY minimizer generation coordinator input is invalid");
    }
    if (backend != backend_type::scalar && backend != backend_type::exact_simd &&
        backend != backend_type::bounded_nthash_simd && backend != backend_type::vulkan_segment) {
        ready_minimizer_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE,
                                     "READY minimizer generation backend is invalid");
    }
    const std::uint16_t window_size = algorithm_identity.runtime_window_size;
    const std::uint16_t kmer_size = algorithm_identity.runtime_kmer_size;
    constexpr std::uint64_t k_max_occ_cap =
        bioseq::indexing::k_grouped_soa_default_max_occurrence_cap;
    constexpr std::uint16_t k_alphabet_code = source_binding_type::alphabet_code();
    constexpr std::uint64_t k_fragment_span_bp = source_binding_type::fragment_bp_capacity();
    static_assert(k_fragment_span_bp != 0);
    const std::uint64_t persisted_segment_span_bp =
        is_fragment_index ? k_fragment_span_bp
                          : bioseq::indexing::k_minimizer_preferred_segment_span_bp;
    const std::uint64_t algorithm_position_extent_bp =
        is_fragment_index ? bioseq::indexing::k_minimizer_fragment_algorithm_position_extent_bp
                          : locator.logical_length;
    const auto selected_algorithm_identity =
        bioseq::indexing::select_minimizer_generation_algorithm_identity_runtime(
            algorithm_identity.requested_algorithm,
            algorithm_position_extent_bp,
            k_alphabet_code,
            static_cast<std::uint8_t>(source_binding_type::storage_unit_byte_width() * 8u),
            kmer_size,
            window_size);
    if (!selected_algorithm_identity ||
        *selected_algorithm_identity != algorithm_identity ||
        !bioseq::indexing::is_valid_minimizer_generation_algorithm_identity(
            algorithm_identity,
            bioseq::indexing::minimizer_producer_backend_identity_for_backend(backend)) ||
        !bioseq::indexing::minimizer_generation_kernel_binding_is_valid(
            backend, algorithm_identity, kernels)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "READY minimizer generation algorithm identity is unavailable");
    }
    if (static_cast<std::uint64_t>(sequence.bp_size()) != locator.logical_length) {
        ready_minimizer_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "READY minimizer lazy sequence shape disagrees with its locator");
    }

    const relation_identity relation_identity = bioseq::postgres::support::invoke_pg_as_cpp(
        [user_relid] { return index_relation_identity(user_relid); });
    if (relation_identity.db_oid != MyDatabaseId || !OidIsValid(relation_identity.tablespace_oid) ||
        !OidIsValid(relation_identity.relfilenode) || storage.db_oid_ != relation_identity.db_oid ||
        storage.tablespace_oid_ != relation_identity.tablespace_oid ||
        storage.main_relfilenode_ != relation_identity.relfilenode ||
        storage.user_relid_ != user_relid || storage.column_attno_ != column_attno ||
        storage.index_name_ != index_name || storage.bucket_idx_ != locator.bucket_index ||
        storage.shard_idx_ != locator.shard_index || storage.query_key_ != sequence_rowno ||
        source_binding.db_oid() != relation_identity.db_oid ||
        source_binding.tablespace_oid() != relation_identity.tablespace_oid ||
        source_binding.main_relfilenode() != relation_identity.relfilenode) {
        ready_minimizer_error_as_cpp(ERRCODE_DATA_CORRUPTED,
                                     "READY minimizer storage identity disagrees with its source");
    }

    const bioseq::indexing::minimizer_scheme_identity scheme =
        bioseq::indexing::minimizer_scheme_identity_for_backend(backend);
    const bioseq::indexing::minimizer_producer_identity producer_backend =
        bioseq::indexing::minimizer_producer_backend_identity_for_backend(backend);
    bioseq::indexing::grouped_soa_minimizer_profile profile{};
    {
        const auto derived_profile = bioseq::indexing::derive_grouped_soa_minimizer_profile(
            scheme, k_alphabet_code, kmer_size, persisted_segment_span_bp, k_max_occ_cap);
        if (!derived_profile) {
            ready_minimizer_error_as_cpp(
                ERRCODE_FEATURE_NOT_SUPPORTED,
                "READY minimizer backend has no canonical grouped-SoA profile",
                fmt::format("backend={} wire_error={}",
                            bioseq::minimizers::minimizer_sketch_backend_name(backend),
                            static_cast<unsigned>(derived_profile.error())));
        }
        profile = *derived_profile;
    }

    struct build_state {
        minimizer_generation_administration_identity identity{};
        std::optional<minimizer_generation_source_authorization> source_authorization;
        minimizer_generation_source_locator source{};
        std::uint64_t source_content_checksum = 0;
        std::optional<index_storage_fragment_generation_row_sink> writer;
        std::optional<bioseq::indexing::encoded_grouped_soa_generation> generation;
        std::optional<ready_minimizer_generation_token> ready_token;
        std::optional<bioseq::indexing::minimizer_generation_encoding_error> encode_error;
        bioseq::indexing::minimizer_fragment_generation_execution_receipt fragment_execution{};
        bioseq::indexing::minimizer_sequence_generation_execution_receipt sequence_execution{};
        ready_minimizer_generation_execution_receipt execution{};
        bool sequence_spill_started = false;
    };
    auto* state = new build_state{
        .identity =
            minimizer_generation_administration_identity{
                .database_oid = relation_identity.db_oid,
                .tablespace_oid = relation_identity.tablespace_oid,
                .main_relfilenode = relation_identity.relfilenode,
                .user_relid = user_relid,
                .column_attno = column_attno,
                .index_name = std::string{index_name},
                .table_name = storage.this_table_name(),
            },
    };
    struct publication_draft {
        ready_minimizer_generation_index_kind index_kind =
            ready_minimizer_generation_index_kind::fragment;
        bioseq::indexing::minimizer_generation_publication_binding binding{};
        std::uint64_t generation_id = 0;
        std::uint64_t source_identity = 0;
        std::uint64_t source_content_checksum = 0;
        std::uint64_t storage_contract_checksum = 0;
        std::uint64_t control_count = 0;
        std::uint64_t position_count = 0;
        std::uint64_t writer_rows = 0;
        std::uint64_t writer_batches = 0;
        bioseq::minimizers::minimizer_sketch_backend selected_backend =
            bioseq::minimizers::minimizer_sketch_backend::scalar;
        ready_minimizer_generation_execution_receipt execution{};
        std::optional<std::uint64_t> replaced_generation_id{};
        std::uint64_t minimizer_sequence_spill_generation_token = 0;
        std::uint64_t minimizer_sequence_spill_mutation_before_release = 0;
        std::uint64_t minimizer_sequence_spill_mutation_after_release = 0;
        bool minimizer_sequence_spill_resources_released = false;
    } result{
        .index_kind = is_fragment_index ? ready_minimizer_generation_index_kind::fragment
                                        : ready_minimizer_generation_index_kind::sequence,
        .selected_backend = backend,
    };
    try {
        scoped_forced_bounded_nthash_index_failure forced_bounded_failure;
        state->source_authorization.emplace(authorize_minimizer_generation_source_pg(
            state->identity, sequence_rowno, registered_source_snapshot));
        state->source = state->source_authorization->source_for_diagnostics();
        state->source_content_checksum =
            state->source_authorization->source_content_checksum_for_diagnostics();
        if (state->source.sequence_rowno != sequence_rowno ||
            !same_minimizer_generation_sequence_locator(state->source.locator, locator) ||
            state->source.alphabet_code != source_binding_type::alphabet_code() ||
            state->source.storage_unit_byte_width !=
                source_binding_type::storage_unit_byte_width() ||
            state->source.padding_letters != source_binding.last_fragment_padding() ||
            state->source.locator.logical_length != sequence.bp_size() ||
            state->source.locator.fragment_count != locator.fragment_count) {
            ready_minimizer_error_as_cpp(ERRCODE_DATA_CORRUPTED,
                                         "READY minimizer authoritative source authorization "
                                         "disagrees with the passed lazy sequence");
        }
        state->source_authorization.reset();

        // Source identity is the immutable source epoch. The storage
        // contract is derived from the encoder's final manifest because the
        // planner may legitimately retry with a smaller fixed-width span.
        result.source_identity =
            minimizer_generation_source_identity(relation_identity.relfilenode, state->source);
        result.source_content_checksum = state->source_content_checksum;
        if (result.source_identity == 0) {
            ready_minimizer_error_as_cpp(ERRCODE_DATA_CORRUPTED,
                                         "READY minimizer source identity is zero");
        }

        ensure_vulkan_index_build_runtime_ready(backend);
        result.generation_id = bioseq::postgres::support::invoke_pg_as_cpp(
            [] { return allocate_minimizer_generation_id_pg(); });
        state->writer.emplace(storage.begin_generation_write(sequence_rowno, result.generation_id));
        forced_bounded_failure.arm_after_publisher_state_begin(backend);
        const bioseq::indexing::grouped_soa_generation_options options{
            .layout = bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local,
            .generation_id = result.generation_id,
            .source_identity = result.source_identity,
            .source_content_checksum = state->source_content_checksum,
            .target_length = locator.logical_length,
            .position_segment_span_bp = persisted_segment_span_bp,
            .alphabet_code = state->source.alphabet_code,
            .storage_unit_byte_width = state->source.storage_unit_byte_width,
            .kmer_size = kmer_size,
            .window_size = window_size,
            .minimizer_scheme_id = scheme.id,
            .minimizer_scheme_revision = scheme.revision,
            .algorithm_identity = algorithm_identity,
            .max_occ_cap = k_max_occ_cap,
        };
        const bioseq::indexing::cooperative_cancellation_checkpoint
            generation_cancellation{
                .context = nullptr,
                .invoke_fn = &detail::check_generation_interrupts_as_cpp,
            };
        detail::check_generation_interrupts_as_cpp(nullptr);
        if (is_fragment_index) {
            auto encoded = fragment_index::encode_persisted_generation(
                sequence,
                backend,
                kernels,
                *state->writer,
                options,
                bioseq::indexing::grouped_soa_generation_limits{},
                &state->fragment_execution,
                state->source.alphabet_code,
                storage.run_buffer_bytes(),
                generation_cancellation);
            if (encoded) {
                state->generation.emplace(std::move(*encoded));
                state->execution = state->fragment_execution;
            } else {
                state->encode_error = encoded.error();
            }
        } else {
            state->sequence_spill_started = true;
            auto spill = sequence_index::spill_generation_replay_runs_serial(
                storage,
                sequence,
                backend,
                kernels,
                algorithm_identity,
                /*core_span_bp=*/0,
                bioseq::indexing::k_minimizer_preferred_segment_span_bp,
                &state->sequence_execution,
                state->source.alphabet_code);
            if (!spill) {
                state->encode_error = spill.error();
            } else {
                auto encoded = sequence_index::encode_persisted_generation(
                    storage,
                    *state->writer,
                    std::move(*spill),
                    options,
                    bioseq::indexing::grouped_soa_generation_limits{},
                    &state->sequence_execution);
                if (encoded) {
                    state->generation.emplace(std::move(*encoded));
                    state->execution = state->sequence_execution;
                } else {
                    state->encode_error = encoded.error();
                }
            }
        }
        detail::check_generation_interrupts_as_cpp(nullptr);
        if (state->generation.has_value()) {
            result.binding =
                bioseq::indexing::make_minimizer_generation_publication_binding(*state->generation);
        }

        // PostgreSQL owns every sequence-index spill run. Destroy those BufFile/run
        // resources after the encoder has copied out its immutable validation state,
        // and before any READY token can be validated or published.
        if (is_sequence_index) {
            const auto before_release = storage.segment_spill_storage_binding();
            if (!state->encode_error &&
                (!state->generation.has_value() ||
                 !state->sequence_execution.allows_ready_release(before_release))) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "READY minimizer sequence spill incarnation changed before release");
            }
            storage.release_runs();
            const auto after_release = storage.segment_spill_storage_binding();
            std::uint64_t retained_groups = 0;
            storage.merge_segment_runs([&retained_groups](auto&&...) { ++retained_groups; });
            result.minimizer_sequence_spill_generation_token = before_release.generation_token;
            result.minimizer_sequence_spill_mutation_before_release =
                before_release.mutation_revision;
            result.minimizer_sequence_spill_mutation_after_release =
                after_release.mutation_revision;
            result.minimizer_sequence_spill_resources_released =
                retained_groups == 0 && before_release.valid() &&
                after_release.generation_token == before_release.generation_token &&
                before_release.mutation_revision != (std::numeric_limits<std::uint64_t>::max)() &&
                after_release.mutation_revision == before_release.mutation_revision + 1u;
            state->sequence_spill_started = false;
        }
        if (state->encode_error) {
            if (backend == bioseq::minimizers::minimizer_sketch_backend::bounded_nthash_simd &&
                *state->encode_error ==
                    bioseq::indexing::minimizer_generation_encoding_error::admission) {
                throw bioseq::indexing::packed_bounded_nthash_index_runtime_error{
                    bioseq::indexing::packed_bounded_nthash_index_status::admission_denied,
                    is_fragment_index ? "minimizer fragment generation encode"
                                      : "minimizer sequence generation encode"};
            }
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "READY minimizer generation encoder failed",
                fmt::format("backend={} wire_error={}",
                            bioseq::minimizers::minimizer_sketch_backend_name(backend),
                            static_cast<unsigned>(*state->encode_error)));
        }
        bool execution_rejected = false;
        std::uint64_t producer_scans = 0;
        std::uint64_t geometry_retries = 0;
        std::uint64_t scalar_runs = 0;
        std::uint64_t exact_runs = 0;
        std::uint64_t bounded_runs = 0;
        std::uint64_t vulkan_runs = 0;
        std::uint64_t vulkan_roundtrips = 0;
        if (const auto* execution =
                std::get_if<bioseq::indexing::minimizer_fragment_generation_execution_receipt>(
                    &state->execution)) {
            execution_rejected =
                !execution->publication_accepted(result.binding) || !is_fragment_index ||
                execution->selected_backend != backend || execution->minimizer_scheme != scheme ||
                execution->target_length != locator.logical_length ||
                execution->window_size != window_size || execution->kmer_size != kmer_size ||
                execution->algorithm_identity != algorithm_identity;
            producer_scans = execution->producer_scan_count;
            geometry_retries = execution->geometry_retry_count;
            scalar_runs = execution->scalar_core_runs;
            exact_runs = execution->exact_simd_core_runs;
            bounded_runs = execution->bounded_nthash_core_runs;
            vulkan_runs = execution->vulkan_core_runs;
            vulkan_roundtrips = execution->vulkan_stats.completed_roundtrips;
        } else if (const auto* execution = std::get_if<
                       bioseq::indexing::minimizer_sequence_generation_execution_receipt>(
                       &state->execution)) {
            execution_rejected =
                !execution->matches_publication(result.binding) || !is_sequence_index ||
                execution->selected_backend != backend || execution->minimizer_scheme != scheme ||
                execution->target_length != locator.logical_length ||
                execution->window_size != window_size || execution->kmer_size != kmer_size ||
                execution->algorithm_identity != algorithm_identity ||
                !result.minimizer_sequence_spill_resources_released;
            producer_scans = execution->replay_scan_count;
            geometry_retries = execution->replay_reroute_pass_count;
            scalar_runs = execution->scalar_core_runs;
            exact_runs = execution->exact_simd_core_runs;
            bounded_runs = execution->bounded_nthash_core_runs;
            vulkan_runs = execution->vulkan_core_runs;
            vulkan_roundtrips = execution->vulkan_stats.completed_roundtrips;
        } else {
            execution_rejected = true;
        }
        if (execution_rejected) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "READY minimizer selected backend execution stats were rejected",
                fmt::format("backend={} scans={} retries={} scalar={} exact={} bounded={} "
                            "vulkan={} vulkan_roundtrips={}",
                            bioseq::minimizers::minimizer_sketch_backend_name(backend),
                            producer_scans,
                            geometry_retries,
                            scalar_runs,
                            exact_runs,
                            bounded_runs,
                            vulkan_runs,
                            vulkan_roundtrips));
        }
        const auto& manifest = state->generation->manifest;
        if (result.binding !=
            bioseq::indexing::make_minimizer_generation_publication_binding(*state->generation)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "READY minimizer publication binding changed before validation");
        }
        result.storage_contract_checksum =
            bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(manifest);
        state->source.storage_contract_checksum = result.storage_contract_checksum;
        if (manifest.layout !=
                bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local ||
            manifest.generation_id != result.generation_id ||
            manifest.source_identity != result.source_identity ||
            manifest.source_content_checksum != state->source_content_checksum ||
            manifest.target_length != locator.logical_length ||
            manifest.alphabet_code != state->source.alphabet_code ||
            manifest.storage_unit_byte_width != state->source.storage_unit_byte_width ||
            manifest.kmer_size != kmer_size || manifest.window_size != window_size ||
            manifest.minimizer_scheme_id != scheme.id ||
            manifest.minimizer_scheme_revision != scheme.revision ||
            manifest.producer_backend_id != producer_backend.id ||
            manifest.producer_identity_revision != producer_backend.revision ||
            manifest.algorithm_identity != algorithm_identity ||
            manifest.gpu_abi_revision != profile.gpu_abi_revision ||
            manifest.max_occ_cap != k_max_occ_cap || manifest.position_segment_span_bp == 0 ||
            (is_fragment_index ? manifest.position_segment_span_bp != k_fragment_span_bp
                               : manifest.position_segment_span_bp >
                                     bioseq::indexing::k_minimizer_preferred_segment_span_bp) ||
            state->generation->head.storage_contract_checksum != result.storage_contract_checksum ||
            state->generation->head.source_identity != result.source_identity ||
            state->generation->head.source_content_checksum != state->source_content_checksum) {
            ready_minimizer_error_as_cpp(ERRCODE_DATA_CORRUPTED,
                                         "READY minimizer encoded manifest identity disagrees");
        }

        detail::check_generation_interrupts_as_cpp(nullptr);
        state->ready_token.emplace(state->writer->validate_ready_generation(*state->generation));
        result.writer_rows = state->writer->row_count();
        result.writer_batches = state->writer->catalog_multi_insert_batches();
        result.control_count = manifest.control_count;
        result.position_count = manifest.position_count;
        state->generation.reset();
        state->writer.reset();
        storage.close_checked();
        inject_minimizer_generation_failure_as_cpp(
            minimizer_generation_failure_injection::generation_storage_close,
            "generation_storage_close");

        detail::check_generation_interrupts_as_cpp(nullptr);
        ready_minimizer_generation_published_head_receipt publication_receipt =
            publish_ready_minimizer_generation_head_pg(std::move(*state->ready_token));
        state->ready_token.reset();
        result.replaced_generation_id = publication_receipt.replaced_generation_id;

        bioseq::postgres::support::invoke_pg_as_cpp([] { CommandCounterIncrement(); });
        bioseq::postgres::support::invoke_pg_as_cpp([] { UpdateActiveSnapshotCommandId(); });
        Snapshot verification_source =
            bioseq::postgres::support::invoke_pg_as_cpp([] { return GetActiveSnapshot(); });
        Snapshot verification_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [verification_source] { return RegisterSnapshot(verification_source); });
        const auto reread_expected =
            bioseq::indexing::expected_grouped_soa_generation_identity_for_backend(
                bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local,
                backend,
                algorithm_identity,
                state->source.alphabet_code,
                state->source.storage_unit_byte_width,
                kmer_size,
                window_size,
                k_max_occ_cap);
        if (!reread_expected) {
            ready_minimizer_error_as_cpp(ERRCODE_DATA_CORRUPTED,
                                         "READY minimizer publication reread contract is invalid");
        }
        {
            index_storage_fragment_generation_read_session reader{
                publication_receipt.table_name,
                publication_receipt.query_key,
                verification_snapshot,
                *reread_expected,
                state->identity.tablespace_oid,
                minimizer_generation_snapshot_registration::borrow_registered};
            const minimizer_generation_head reread_head = reader.read_active_head_once();
            const auto reread_head_bytes =
                bioseq::indexing::encode_minimizer_generation_head(reread_head);
            if (!reread_head_bytes || reread_head != publication_receipt.head ||
                !bioseq::indexing::is_fixed_wire_returned_exactly(publication_receipt.head_bytes,
                                                                  *reread_head_bytes)) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "READY minimizer publication independent head reread disagrees");
            }
            inject_minimizer_generation_failure_as_cpp(
                minimizer_generation_failure_injection::generation_post_publish_reread,
                "generation_post_publish_reread");
            reader.close_head_read();
            inject_minimizer_generation_failure_as_cpp(
                minimizer_generation_failure_injection::generation_reader_close,
                "generation_reader_close");
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [verification_snapshot] { UnregisterSnapshot(verification_snapshot); });

        result.execution = state->execution;
        delete state;
        state = nullptr;
    } catch (const bioseq::postgres::support::postgres_error&) {
        storage.abandon_after_pg_error_noexcept();
        state->ready_token.reset();
        state->generation.reset();
        state->sequence_spill_started = false;
        state->writer.reset();
        state->source_authorization.reset();
        delete state;
        state = nullptr;
        throw;
    } catch (...) {
        storage.abandon_after_pg_error_noexcept();
        state->ready_token.reset();
        state->generation.reset();
        state->sequence_spill_started = false;
        state->writer.reset();
        state->source_authorization.reset();
        delete state;
        state = nullptr;
        throw;
    }
    return ready_minimizer_generation_publication{
        result.index_kind,
        result.binding,
        result.control_count,
        result.position_count,
        result.writer_rows,
        result.writer_batches,
        result.selected_backend,
        std::move(result.execution),
        result.replaced_generation_id,
        result.minimizer_sequence_spill_generation_token,
        result.minimizer_sequence_spill_mutation_before_release,
        result.minimizer_sequence_spill_mutation_after_release,
        result.minimizer_sequence_spill_resources_released};
}

/// Seal the update_index result at the caller boundary. The
/// coordinator already validates the producer-originated execution receipt before
/// publication. This function also validates that the returned alternative,
/// backend, and immutable publication binding still match the requested normal
/// rebuild route. There is no scalar/Exact fallback on any selected backend.
template <class p_sequence_range>
inline void build_and_verify_grouped_soa_minimizer_generation_pg(
    Oid user_relid,
    std::int16_t column_attno,
    std::string_view index_name,
    std::uint64_t sequence_rowno,
    fragment_index_storage& storage,
    bioseq::storage::lazy_sequence_view<
        typename bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>::alphabet_type>
        sequence,
    bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range> source_binding,
    const bioseq::indexing::minimizer_generation_algorithm_identity& algorithm_identity,
    bioseq::minimizers::minimizer_sketch_backend backend,
    const bioseq::minimizers::minimizer_kernels* kernels) {
    const ready_minimizer_generation_publication publication =
        build_and_publish_grouped_soa_minimizer_generation_pg(user_relid,
                                                              column_attno,
                                                              index_name,
                                                              sequence_rowno,
                                                              storage,
                                                              sequence,
                                                              source_binding,
                                                              algorithm_identity,
                                                              backend,
                                                              kernels);
    const bool execution_kind_matches =
        index_name == k_minimizers_per_fragment_index_name
            ? std::holds_alternative<
                  bioseq::indexing::minimizer_fragment_generation_execution_receipt>(
                  publication.execution())
            : index_name == k_minimizers_per_sequence_index_name &&
                  std::holds_alternative<
                      bioseq::indexing::minimizer_sequence_generation_execution_receipt>(
                      publication.execution());
    const bool result_kind_matches =
        (index_name == k_minimizers_per_fragment_index_name &&
         publication.index_kind() == ready_minimizer_generation_index_kind::fragment) ||
        (index_name == k_minimizers_per_sequence_index_name &&
         publication.index_kind() == ready_minimizer_generation_index_kind::sequence);
    if (!publication.published() || publication.generation_id() == 0 ||
        publication.source_identity() == 0 || publication.selected_backend() != backend ||
        !publication.execution_accepted() || !execution_kind_matches || !result_kind_matches) {
        ready_minimizer_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "normal minimizer rebuild publication validation was rejected",
            fmt::format("index={} backend={}",
                        index_name,
                        bioseq::minimizers::minimizer_sketch_backend_name(backend)));
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    detail::log_e2e_minimizer_publication_execution(
        publication, user_relid, column_attno, index_name, sequence_rowno);
#endif
}

// Keep one explicit production facade/binding instantiation as a narrow
// compile-time contract in addition to the ordinary update_index call sites.
namespace ready_minimizer_generation_compile_contract_detail {
using sequence_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using source_binding = bioseq::storage::postgres_lazy_sequence_binding<sequence_config>;
using sequence_view = bioseq::storage::lazy_sequence_view<seq::dna4>;
} // namespace ready_minimizer_generation_compile_contract_detail

template ready_minimizer_generation_publication
build_and_publish_grouped_soa_minimizer_generation_pg<
    ready_minimizer_generation_compile_contract_detail::sequence_config>(
    Oid,
    std::int16_t,
    std::string_view,
    std::uint64_t,
    fragment_index_storage&,
    ready_minimizer_generation_compile_contract_detail::sequence_view,
    ready_minimizer_generation_compile_contract_detail::source_binding,
    const bioseq::indexing::minimizer_generation_algorithm_identity&,
    bioseq::minimizers::minimizer_sketch_backend,
    const bioseq::minimizers::minimizer_kernels*);

} // namespace bioseq::storage::indexing
