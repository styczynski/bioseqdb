/**
 * @file typed_support_parts/vulkan_resident_route.cppm
 * @brief Owns resident-target Vulkan build, coordinate, and CIGAR execution.
 */
module;

#include <algorithm>
#include <array>
#include <bioseq/pg_compat.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <libassert/assert.hpp>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support:vulkan_resident_route;

import bioseq.postgres.scheduler.dispatch_processor;
import :policy_stats;
import :pg_alignment_errors;
import :transient_storage;
import :resident_targets;
import :row_adapters;
import :scanner_row;
import :vulkan_query_route;
import bioseq.highway.dispatch_contract;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
import bioseq.common.seqan_adapter;
import bioseq.postgres.support;
import bioseq.postgres.configuration.core;
import bioseq.postgres.configuration.scheduler;
import bioseq.indexing.type;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_hashmap;
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
import bioseq.instrumentation.configuration;
import bioseq.instrumentation.descriptor;
import bioseq.instrumentation.profiler_state;
import bioseq.instrumentation.region;
import bioseq.instrumentation.runtime;
import bioseq.postgres.storage.manager;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.storage.layout;
import bioseq.postgres.storage.scanner;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.scheduler.supervisor.vulkan;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/**
 * Describes one logical target and every admitted resident segment.
 */
template <class p_gpu_row_view> struct gpu_batch_target_sidecar {
    const bioseq::pg_types::seq* hdr = nullptr;
    std::optional<p_gpu_row_view> recipe_view{};
    Datum recipe_datum = Datum{0};
    std::size_t source_row_index = 0u;
    std::uint64_t fingerprint = 0;
    sequence_size_t target_len = 0;
    sequence_size_t max_query_len = 0;
    std::size_t row_count = 0;
    std::vector<::bioseq::indexing::segment_unit_range> ranges{};
    std::vector<std::shared_ptr<bioseq::indexing::gpu_resident_minimizer_index>> residents{};
};

/**
 * Maps one submitted coordinate problem back to its logical row and segment.
 */
struct gpu_batch_problem_sidecar {
    std::size_t row_index = 0;
    std::size_t target_index = 0;
    std::size_t segment_index = 0;
    sequence_size_t core_lo_bp = 0;
    sequence_size_t core_hi_bp = 0;
};

/**
 * Executes transient resident target preparation, batched coordinates, and
 * optional bounded GPU CIGAR projection for one admitted Vulkan batch.
 */
template <class p_sequence, class p_runtime>
void process_vulkan_resident_route(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>& target_hdrs,
    const std::array<int32_t, dispatch_batch_capacity>& query_ids,
    const minimap2_parameters& dispatch_params,
    const std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
    batch_processor::scanner_pg_context* scanner_pg_ctx,
    const bool per_row_query_mode,
    const bool record_target_operational_outcomes,
    bool& cached_query_minimizer_stats_accounted,
    p_runtime& runtime,
    vulkan_query_batch_state<p_sequence>& state,
    std::map<int32_t, std::vector<match_result>>& results) {
    using minimizer_type = typename vulkan_query_batch_state<p_sequence>::minimizer_type;
    using gpu_row_view = typename vulkan_query_batch_state<p_sequence>::gpu_row_view;
    using target_sidecar_type = gpu_batch_target_sidecar<gpu_row_view>;
    auto& gpu_query_recipe_views = state.gpu_query_recipe_views;
    auto& gpu_target_recipe_views = state.gpu_target_recipe_views;
    const std::size_t w = state.w;
    const std::size_t k = state.k;
    auto& query_records = state.query_records;
    auto& query_minimizer_device_span = state.query_minimizer_device_span;
    auto& query_sketch_authority = state.query_sketch_authority;
    auto& query_sidecars = state.query_sidecars;
    auto& row_to_query_index = state.row_to_query_index;
    auto& query_gate = state.query_gate;
    const auto gpu_batch_now_ns = []() -> std::uint64_t {
        return static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
    };
    add_stage_host_time(
        stats, ::bioseq::pipeline_stats::pipeline_stage::query_sketch, gpu_query_sketch_host_ns);

    std::vector<target_sidecar_type> targets;
    std::vector<std::uint32_t> row_to_target_index(batch_size,
                                                   std::numeric_limits<std::uint32_t>::max());
    targets.reserve(batch_size);
    const auto target_storage_bytes = [](const bioseq::pg_types::seq* hdr) -> std::string_view {
        compact_sequence_range<p_sequence> seq{*hdr};
        using storage_unit_type = typename compact_sequence_range<p_sequence>::traits::storage_unit;
        const auto* bytes = reinterpret_cast<const char*>(seq.cbegin());
        return std::string_view{bytes, seq.size() * sizeof(storage_unit_type)};
    };
    const auto target_fingerprint =
        [target_storage_bytes](const bioseq::pg_types::seq* hdr) -> std::uint64_t {
        std::uint64_t hash = 14695981039346656037ull;
        hash =
            bioseq::common::hash_combine_64(hash, static_cast<std::uint64_t>(hdr->alphabet_code));
        hash = bioseq::common::hash_combine_64(
            hash, static_cast<std::uint64_t>(hdr->storage_unit_byte_width));
        hash = bioseq::common::hash_combine_64(hash, static_cast<std::uint64_t>(hdr->bp_size));
        return bioseq::common::hash_combine_64(hash, target_storage_bytes(hdr));
    };
    for (std::size_t row_index = 0; row_index < batch_size; ++row_index) {
        if (row_to_query_index[row_index] == std::numeric_limits<std::uint32_t>::max()) {
            continue;
        }
        const bioseq::pg_types::seq* target_hdr = target_hdrs[row_index];
        const bool recipe_target = gpu_target_recipe_views[row_index].has_value();
        const Datum target_recipe_datum =
            recipe_target ? scanner_pg_ctx->target_recipe_datums[row_index] : Datum{0};
        if (target_hdr != nullptr && recipe_target) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan target recipe overlaps a flat target");
        }
        if (target_hdr == nullptr && !recipe_target) {
            if (scanner_pg_ctx != nullptr &&
                scanner_pg_ctx->target_locators[row_index].has_value()) {
                const auto& loc = *scanner_pg_ctx->target_locators[row_index];
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    fmt::format("selected Vulkan batch target row {} "
                                "is only available as a PG locator "
                                "({} bp); selected Vulkan requires a "
                                "materialized target payload and "
                                "cannot use scanner_pg/MaxAllocSize "
                                "streaming",
                                static_cast<int>(query_ids[row_index]),
                                static_cast<unsigned long long>(loc.logical_length)));
            }
            continue;
        }
        if (target_hdr != nullptr &&
            (target_hdr->alphabet_code != alphabet_code ||
             target_hdr->storage_unit_byte_width != storage_unit_byte_width)) {
            continue;
        }
        std::uint32_t target_index = std::numeric_limits<std::uint32_t>::max();
        for (std::size_t existing = 0; existing < targets.size(); ++existing) {
            const bool same_source_recipe = recipe_target && target_recipe_datum != Datum{0} &&
                                            targets[existing].recipe_datum == target_recipe_datum;
            const bool same_source_locator =
                recipe_target && target_recipe_datum == Datum{0} &&
                targets[existing].recipe_view.has_value() &&
                scanner_pg_ctx->target_locators[row_index].has_value() &&
                scanner_pg_ctx->target_locators[targets[existing].source_row_index].has_value() &&
                same_dispatch_target_locator(
                    *scanner_pg_ctx->target_locators[row_index],
                    *scanner_pg_ctx->target_locators[targets[existing].source_row_index]);
            if ((!recipe_target && targets[existing].hdr == target_hdr) || same_source_recipe ||
                same_source_locator) {
                target_index = static_cast<std::uint32_t>(existing);
                break;
            }
        }
        std::uint64_t fingerprint = 0u;
        if (target_index == std::numeric_limits<std::uint32_t>::max() && !recipe_target) {
            fingerprint = target_fingerprint(target_hdr);
        }
        for (std::size_t existing = 0; target_index == std::numeric_limits<std::uint32_t>::max() &&
                                       !recipe_target && existing < targets.size();
             ++existing) {
            if (targets[existing].fingerprint == fingerprint &&
                targets[existing].target_len == static_cast<sequence_size_t>(target_hdr->bp_size) &&
                targets[existing].hdr != nullptr &&
                target_storage_bytes(targets[existing].hdr) == target_storage_bytes(target_hdr)) {
                target_index = static_cast<std::uint32_t>(existing);
                break;
            }
        }
        const sequence_size_t row_query_len =
            query_sidecars[row_to_query_index[row_index]].query_len;
        const sequence_size_t target_length =
            recipe_target ? gpu_target_recipe_views[row_index]->bp_size()
                          : static_cast<sequence_size_t>(target_hdr->bp_size);
        if (target_index == std::numeric_limits<std::uint32_t>::max()) {
            target_index = static_cast<std::uint32_t>(targets.size());
            targets.push_back(target_sidecar_type{
                .hdr = target_hdr,
                .recipe_view = recipe_target ? gpu_target_recipe_views[row_index] : std::nullopt,
                .recipe_datum = target_recipe_datum,
                .source_row_index = row_index,
                .fingerprint = fingerprint,
                .target_len = target_length,
                .max_query_len = row_query_len,
                .row_count = 1,
                .ranges = {},
                .residents = {},
            });
        } else {
            ++targets[target_index].row_count;
            targets[target_index].max_query_len =
                std::max(targets[target_index].max_query_len, row_query_len);
        }
        row_to_target_index[row_index] = target_index;
    }
    const auto requested_source = requested_target_index_source(scanner_pg_ctx);
    const auto record_gpu_batch_outcomes = [batch_size,
                                            broadcast_query_ordinal,
                                            per_row_query_mode,
                                            record_target_operational_outcomes,
                                            requested_source,
                                            scanner_pg_ctx,
                                            &dispatch_params,
                                            &row_to_query_index,
                                            &row_to_target_index,
                                            &cached_query_minimizer_stats_accounted,
                                            &stats] {
        std::vector<std::uint64_t> logical_target_rows;
        if (record_target_operational_outcomes) {
            logical_target_rows.reserve(batch_size);
        }
        for (std::size_t row = 0; row < batch_size; ++row) {
            if (row_to_query_index[row] == std::numeric_limits<std::uint32_t>::max() ||
                row_to_target_index[row] == std::numeric_limits<std::uint32_t>::max()) {
                continue;
            }
            if (record_target_operational_outcomes) {
                const std::uint64_t logical_target_row =
                    scanner_pg_ctx != nullptr && scanner_pg_ctx->target_rownos[row] >= 0
                        ? static_cast<std::uint64_t>(scanner_pg_ctx->target_rownos[row])
                        : static_cast<std::uint64_t>(row);
                logical_target_rows.push_back(logical_target_row);
            }
            record_unattested_job(stats,
                                  dispatch_params,
                                  requested_source,
                                  ::bioseq::pipeline_stats::target_index_source::recomputed,
                                  scanner_pg_ctx,
                                  row,
                                  per_row_query_mode,
                                  broadcast_query_ordinal,
                                  /*execution_stats=*/nullptr,
                                  /*consumed_shared_cached_query_stats=*/false,
                                  cached_query_minimizer_stats_accounted);
        }
        if (record_target_operational_outcomes) {
            const auto unique_target_count =
                ::bioseq::pipeline_stats::count_unique_logical_target_rows(
                    std::span<std::uint64_t>{logical_target_rows});
            record_target_row(stats, unique_target_count);
            record_target_index_outcome(stats,
                                        requested_source,
                                        ::bioseq::pipeline_stats::target_index_source::recomputed,
                                        unique_target_count);
        }
    };
    if (query_records.empty() || targets.empty()) {
        record_gpu_batch_outcomes();
        return;
    }

    const auto frozen_query_gate = pipe::freeze_gpu_query_gate(query_gate.vulkan_gate());
    const auto frozen_gate_identity = frozen_query_gate.canonical_identity();

    const std::size_t lpu = facade_letters_per_unit;
    for (std::size_t target_index = 0; target_index < targets.size(); ++target_index) {
        target_sidecar_type& target = targets[target_index];
        std::optional<compact_sequence_range<p_sequence>> flat_target_sequence;
        const gpu_row_view target_view = [&target, &flat_target_sequence] {
            if (target.recipe_view.has_value()) {
                return *target.recipe_view;
            }
            if (target.hdr == nullptr) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "Vulkan target sidecar has no backing view");
            }
            flat_target_sequence.emplace(*target.hdr);
            return bioseq::storage::make_lazy_sequence_view(*flat_target_sequence);
        }();
        const std::uint64_t gpu_target_plan_begin_ns = gpu_batch_now_ns();
        constexpr sequence_size_t gpu_batch_huge_target_bp = 100'000'000ull;
        constexpr sequence_size_t gpu_batch_huge_target_core_bp = 8'000'000ull;
        constexpr sequence_size_t gpu_batch_huge_target_overlap_max_bp = 30'000'000ull;
        if (target.row_count > 1 && target.target_len >= gpu_batch_huge_target_bp) {
            target.ranges.clear();
            const sequence_size_t total_units = (target.target_len + lpu - 1u) / lpu;
            sequence_size_t overlap_bp =
                target.max_query_len + target.max_query_len / 4u + 100'000ull;
            overlap_bp = std::min(overlap_bp, gpu_batch_huge_target_overlap_max_bp);
            const sequence_size_t overlap_units = (overlap_bp + lpu - 1u) / lpu;
            const sequence_size_t segment_units =
                std::max<sequence_size_t>(1u, gpu_batch_huge_target_core_bp / lpu);
            for (sequence_size_t lo_u = 0; lo_u < total_units; lo_u += segment_units) {
                const sequence_size_t hi_u = std::min(lo_u + segment_units, total_units);
                const sequence_size_t cs_u = lo_u > overlap_units ? lo_u - overlap_units : 0u;
                const sequence_size_t ce_u = std::min(hi_u + overlap_units, total_units);
                target.ranges.push_back(::bioseq::indexing::segment_unit_range{
                    .cs_u = cs_u,
                    .ce_u = ce_u,
                    .core_lo_bp = lo_u * lpu,
                    .core_hi_bp = std::min(hi_u * lpu, target.target_len),
                });
                if (hi_u == total_units) {
                    break;
                }
            }
        } else {
            target.ranges =
                pipe::segment_unit_ranges_for_backend(target.target_len,
                                                      target.max_query_len,
                                                      lpu,
                                                      minimizer_sketch_backend::vulkan_segment);
        }
        add_stage_host_time(stats,
                            ::bioseq::pipeline_stats::pipeline_stage::target_index_build,
                            gpu_batch_now_ns() - gpu_target_plan_begin_ns);
        target.residents.reserve(target.ranges.size());
        for (std::size_t segment_index = 0; segment_index < target.ranges.size(); ++segment_index) {
            const ::bioseq::indexing::segment_unit_range& range = target.ranges[segment_index];
            const auto [segment_lo_bp, segment_hi_bp] =
                pipe::validated_segment_bp_bounds(range, lpu, target.target_len);
            const std::uint64_t gpu_index_build_begin_ns = gpu_batch_now_ns();
            const bool full_target_range =
                segment_lo_bp == 0u && segment_hi_bp == target.target_len;
            bioseq::gpu::vulkan::vulkan_sketch_decline decline{};
            auto built =
                full_target_range
                    ? bioseq::gpu::vulkan::build_gpu_index_on_device<minimizer_type>(
                          target_view, w, k, frozen_query_gate, &decline)
                    : bioseq::gpu::vulkan::build_gpu_segment_index_on_device<minimizer_type>(
                          target_view,
                          segment_lo_bp,
                          segment_hi_bp,
                          w,
                          k,
                          frozen_query_gate,
                          &decline);
            if (!built) {
                pipe::throw_vulkan_sketch_declined("Vulkan batched segment index build", decline);
            }
            if (!built->valid || built->position_count == 0u || built->occupied_slots == 0u) {
                add_stage_host_time(stats,
                                    ::bioseq::pipeline_stats::pipeline_stage::target_index_build,
                                    gpu_batch_now_ns() - gpu_index_build_begin_ns);
                target.residents.push_back(nullptr);
                continue;
            }
            bioseq::indexing::gpu_minimizer_index_cache_key key{
                .source_id = static_cast<std::uint64_t>(target_index + 1u),
                .caller_identity =
                    0xd1b54a32d192ed03ULL ^
                    (static_cast<std::uint64_t>(segment_index) + 0x9e3779b97f4a7c15ULL),
                .target_len = built->target_len,
                .table_size = built->occupied_slots,
                .position_count = built->position_count,
            };
            pipe::bind_verified_gpu_segment_gate_identity(
                key, frozen_gate_identity, built->gate_identity);
            auto adopted = bioseq::indexing::adopt_gpu_built_index(runtime, std::move(*built), key);
            if (!adopted) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
                    "Vulkan batched segment index adoption "
                    "rejected ({})",
                    bioseq::indexing::gpu_built_index_adoption_error_name(adopted.error())));
            }
            auto resident = std::move(*adopted);
            resident->target_position_offset = segment_lo_bp;
            target.residents.push_back(resident);
            add_stage_host_time(stats,
                                ::bioseq::pipeline_stats::pipeline_stage::target_index_build,
                                gpu_batch_now_ns() - gpu_index_build_begin_ns);
        }
    }

    std::vector<bioseq::gpu::vulkan::gpu_seed_coordinate_job> gpu_jobs;
    std::vector<gpu_batch_problem_sidecar> problem_sidecars;
    const std::uint64_t gpu_problem_build_begin_ns = gpu_batch_now_ns();
    gpu_jobs.reserve(batch_size);
    problem_sidecars.reserve(batch_size);
    for (std::size_t target_index = 0; target_index < targets.size(); ++target_index) {
        target_sidecar_type& target = targets[target_index];
        for (std::size_t segment_index = 0; segment_index < target.residents.size();
             ++segment_index) {
            const auto& resident = target.residents[segment_index];
            if (!resident) {
                continue;
            }
            const ::bioseq::indexing::segment_unit_range& range = target.ranges[segment_index];
            for (std::size_t row = 0; row < batch_size; ++row) {
                const std::uint32_t query_index = row_to_query_index[row];
                if (query_index == std::numeric_limits<std::uint32_t>::max() ||
                    row_to_target_index[row] != target_index) {
                    continue;
                }
                gpu_jobs.push_back(bioseq::gpu::vulkan::gpu_seed_coordinate_job{
                    .slots = &resident->slots,
                    .positions = &resident->positions,
                    .target_len = resident->target_len,
                    .slot_mask = resident->slot_mask,
                    .position_count = resident->position_count,
                    .bucket_count = resident->bucket_count,
                    .target_position_offset = resident->target_position_offset,
                    .query_index = query_index,
                });
                problem_sidecars.push_back(gpu_batch_problem_sidecar{
                    .row_index = row,
                    .target_index = target_index,
                    .segment_index = segment_index,
                    .core_lo_bp = range.core_lo_bp,
                    .core_hi_bp = range.core_hi_bp,
                });
            }
        }
    }
    add_stage_host_time(stats,
                        ::bioseq::pipeline_stats::pipeline_stage::seed_lookup,
                        gpu_batch_now_ns() - gpu_problem_build_begin_ns);

    if (gpu_jobs.empty()) {
        record_gpu_batch_outcomes();
        return;
    }

    auto cp = dispatch_params.make_chaining_parameters();
    std::int32_t max_dist_x = cp.max_gap;
    std::int32_t max_dist_y = cp.max_gap;
    if (max_dist_x < cp.bandwidth) {
        max_dist_x = cp.bandwidth;
    }
    if (max_dist_y < cp.bandwidth) {
        max_dist_y = cp.bandwidth;
    }
    const auto chain_key = bioseq::chaining::choose_vulkan_chaining_pipeline(runtime.features());
    if (query_minimizer_device_span.count() >
        static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error{
            "batched device query minimizer count exceeds ABI"};
    }
    bioseq::gpu::vulkan::gpu_seed_query_inputs query_inputs{
        .host_minimizer_bytes = {},
        .device_minimizer_words = query_minimizer_device_span.words(),
        .minimizer_count = static_cast<std::uint32_t>(query_minimizer_device_span.count()),
        .minimizer_word_kind = static_cast<std::uint32_t>(
            bioseq::gpu::vulkan::minimizer_wire_traits<minimizer_type>::k_word_kind),
        .minimizer_words32_per_record =
            bioseq::gpu::vulkan::minimizer_wire_traits<minimizer_type>::k_words32_per_minimizer,
        .hash_shift = bioseq::gpu::vulkan::minimizer_wire_traits<minimizer_type>::k_hash_shift,
        .pos_bits = bioseq::gpu::vulkan::minimizer_wire_traits<minimizer_type>::k_position_bits,
        .host_query_records =
            std::span<const bioseq::gpu::vulkan::gpu_seed_query_record_wire>{query_records.data(),
                                                                             query_records.size()},
        .device_query_records = nullptr,
    };
    const auto coord_run = runtime.run_seed_to_coordinate_batch_roundtrip(
        query_inputs,
        std::span<const bioseq::gpu::vulkan::gpu_seed_coordinate_job>{gpu_jobs.data(),
                                                                      gpu_jobs.size()},
        static_cast<std::uint32_t>(dispatch_params.kmer_size),
        dispatch_params.max_occ,
        bioseq::gpu::vulkan::gpu_seed_coordinate_algorithm::vulkan_tiled_dp,
        chain_key,
        max_dist_x,
        max_dist_y,
        cp.bandwidth,
        std::min<std::int32_t>(cp.max_chain_iter, 3000),
        cp.gap_penalty,
        cp.skip_penalty,
        cp.max_chain_skip,
        cp.min_chain_score,
        cp.min_seed_count,
        16u);
    add_operational_counter(stats,
                            stats.operational.gpu_seed_dispatches,
                            coord_run.stats.seed_count_dispatches +
                                coord_run.stats.seed_emit_dispatches);
    if (coord_run.stats.coordinate_vote_dispatches != 0u) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "selected Vulkan tiled-DP route executed the "
            "coordinate-vote algorithm");
    }
    add_operational_counter(
        stats, stats.operational.gpu_chain_dispatches, coord_run.stats.chain_dispatches);
    add_operational_counter(stats,
                            stats.operational.gpu_chain_backtrack_dispatches,
                            coord_run.stats.chain_backtrack_dispatches);
    add_operational_counter(stats,
                            stats.operational.gpu_coordinate_vote_dispatches,
                            coord_run.stats.coordinate_vote_dispatches);

    struct gpu_cigar_pending_match {
        int32_t query_id = 0;
        match_result match{};
    };
    std::vector<gpu_cigar_pending_match> pending_cigar_matches;
    std::vector<bioseq::gpu::vulkan::gpu_alignment_cigar_job_wire> cigar_jobs;
    std::vector<std::uint32_t> cigar_query_symbols;
    std::vector<std::uint32_t> cigar_target_symbols;
    std::vector<std::uint8_t> cigar_symbol_scratch;
    std::uint32_t packed_cigar_capacity = 0;
    std::uint32_t traceback_word_capacity = 0;
    pending_cigar_matches.reserve(coord_run.results.size());
    cigar_jobs.reserve(coord_run.results.size());
    const auto gpu_cigar_now_ns = []() -> std::uint64_t {
        return static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
    };
    const std::uint64_t cigar_prepare_begin_ns = gpu_cigar_now_ns();

    for (std::size_t problem_index = 0; problem_index < coord_run.results.size(); ++problem_index) {
        const auto& coord = coord_run.results[problem_index];
        if (coord.is_valid == 0u) {
            continue;
        }
        const gpu_batch_problem_sidecar& owner = problem_sidecars[problem_index];
        if (coord.target_start < owner.core_lo_bp || coord.target_start >= owner.core_hi_bp) {
            continue;
        }
        const std::uint32_t query_index = row_to_query_index[owner.row_index];
        const target_sidecar_type& target = targets[owner.target_index];
        match_result coordinate_match = pipe::gpu_coordinate_to_match_result(
            coord, query_sidecars[query_index].query_len, target.target_len);
        if (!dispatch_params.do_cigar) {
            results[query_ids[owner.row_index]].push_back(std::move(coordinate_match));
            continue;
        }

        const bioseq::pg_types::seq* row_query_hdr =
            per_row_query_mode ? (*per_row_query_hdrs)[owner.row_index] : query_hdr;
        const bool recipe_query = gpu_query_recipe_views[owner.row_index].has_value();
        const bool recipe_target = target.recipe_view.has_value();
        if ((row_query_hdr == nullptr && !recipe_query) ||
            (target.hdr == nullptr && !recipe_target)) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "GPU CIGAR extension received null query/target sequence");
        }
        std::optional<compact_sequence_range<p_sequence>> flat_query_sequence;
        std::optional<compact_sequence_range<p_sequence>> flat_target_sequence;
        const gpu_row_view row_query_seq = [recipe_query,
                                            &gpu_query_recipe_views,
                                            owner_row_index = owner.row_index,
                                            &flat_query_sequence,
                                            row_query_hdr] {
            if (recipe_query) {
                return *gpu_query_recipe_views[owner_row_index];
            }
            flat_query_sequence.emplace(*row_query_hdr);
            return bioseq::storage::make_lazy_sequence_view(*flat_query_sequence);
        }();
        const gpu_row_view target_seq = [recipe_target, &target, &flat_target_sequence] {
            if (recipe_target) {
                return *target.recipe_view;
            }
            flat_target_sequence.emplace(*target.hdr);
            return bioseq::storage::make_lazy_sequence_view(*flat_target_sequence);
        }();
        const sequence_size_t query_len_bp = row_query_seq.bp_size();
        const sequence_size_t target_len_bp = target_seq.bp_size();
        if (coordinate_match.query_start > query_len_bp ||
            coordinate_match.target_start > target_len_bp) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "GPU CIGAR extension coordinate start exceeds sequence length");
        }
        coordinate_match.query_end = std::min(coordinate_match.query_end, query_len_bp);
        coordinate_match.target_end = std::min(coordinate_match.target_end, target_len_bp);
        const sequence_size_t query_span =
            coordinate_match.query_end - coordinate_match.query_start;
        const sequence_size_t target_span =
            coordinate_match.target_end - coordinate_match.target_start;
        if (query_span == 0 || target_span == 0) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "GPU CIGAR extension received empty coordinate span");
        }
        const auto checked_u32 = [](std::uint64_t value,
                                    std::string_view field_name) -> std::uint32_t {
            if (value > std::numeric_limits<std::uint32_t>::max()) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    fmt::format("{} exceeds uint32", field_name));
            }
            return static_cast<std::uint32_t>(value);
        };
        if (query_span > bioseq::gpu::vulkan::k_gpu_alignment_simple_cigar_max_len ||
            target_span > bioseq::gpu::vulkan::k_gpu_alignment_simple_cigar_max_len) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                fmt::format("GPU CIGAR extension simple CIGAR supports spans up to {}, "
                            "got query_span={} target_span={}",
                            bioseq::gpu::vulkan::k_gpu_alignment_simple_cigar_max_len,
                            query_span,
                            target_span));
        }

        const std::uint32_t query_symbol_offset =
            checked_u32(cigar_query_symbols.size(), "GPU CIGAR query symbol offset");
        std::span<const std::uint32_t> query_symbols =
            append_gpu_alignment_symbols(cigar_query_symbols,
                                         cigar_symbol_scratch,
                                         row_query_seq,
                                         coordinate_match.query_start,
                                         coordinate_match.query_end,
                                         false);
        const std::uint32_t target_symbol_offset =
            checked_u32(cigar_target_symbols.size(), "GPU CIGAR target symbol offset");
        std::span<const std::uint32_t> target_symbols =
            append_gpu_alignment_symbols(cigar_target_symbols,
                                         cigar_symbol_scratch,
                                         target_seq,
                                         coordinate_match.target_start,
                                         coordinate_match.target_end,
                                         coordinate_match.is_reverse);
        add_operational_counter(stats, stats.operational.host_query_symbol_packing_calls);
        add_operational_counter(stats, stats.operational.host_target_symbol_packing_calls);

        const std::uint32_t query_len = checked_u32(query_span, "GPU CIGAR query span");
        const std::uint32_t target_len = checked_u32(target_span, "GPU CIGAR target span");
        const std::uint32_t cigar_capacity = std::max<std::uint32_t>(
            1u, checked_u32(query_span + target_span, "GPU CIGAR packed capacity"));
        const bool bounded_dp_candidate =
            query_len <= bioseq::gpu::vulkan::k_gpu_alignment_extension_max_len &&
            target_len <= bioseq::gpu::vulkan::k_gpu_alignment_extension_max_len;
        const bool exact_long_cigar_candidate =
            query_len == target_len &&
            std::equal(query_symbols.begin(), query_symbols.end(), target_symbols.begin());
        const bool unit_gap_walk_cigar_candidate =
            gpu_alignment_unit_gap_walk_cigar_candidate(query_symbols, target_symbols);
        const std::uint64_t traceback_cells =
            bounded_dp_candidate ? (static_cast<std::uint64_t>(query_len) + 1u) *
                                       (static_cast<std::uint64_t>(target_len) + 1u)
            : (exact_long_cigar_candidate || unit_gap_walk_cigar_candidate)
                ? 0u
                : bioseq::gpu::vulkan::gpu_alignment_cigar_gridded_traceback_words(
                      query_len,
                      target_len,
                      bioseq::gpu::vulkan::k_gpu_alignment_cigar_traceback_tile_len,
                      bioseq::gpu::vulkan::k_gpu_alignment_cigar_traceback_tile_len);
        add_operational_counter(stats, stats.operational.host_alignment_job_classification_calls);
        cigar_jobs.push_back(bioseq::gpu::vulkan::gpu_alignment_cigar_job_wire{
            .query_symbol_offset = query_symbol_offset,
            .target_symbol_offset = target_symbol_offset,
            .query_len = query_len,
            .target_len = target_len,
            .query_global_start =
                checked_u32(coordinate_match.query_start, "GPU CIGAR query global start"),
            .target_global_start =
                checked_u32(coordinate_match.target_start, "GPU CIGAR target global start"),
            .band_width = std::min<std::uint32_t>(
                bioseq::gpu::vulkan::k_gpu_alignment_extension_max_band_width,
                std::max(query_len, target_len)),
            .flags = 0,
            .cigar_offset = packed_cigar_capacity,
            .cigar_capacity = cigar_capacity,
            .traceback_tile_offset = traceback_word_capacity,
            .traceback_tile_count = checked_u32(traceback_cells, "GPU CIGAR traceback cell count"),
        });
        packed_cigar_capacity =
            checked_u32(static_cast<std::uint64_t>(packed_cigar_capacity) + cigar_capacity,
                        "GPU CIGAR packed capacity total");
        traceback_word_capacity =
            checked_u32(static_cast<std::uint64_t>(traceback_word_capacity) + traceback_cells,
                        "GPU CIGAR traceback capacity total");
        pending_cigar_matches.push_back(gpu_cigar_pending_match{
            .query_id = query_ids[owner.row_index],
            .match = std::move(coordinate_match),
        });
    }
    add_stage_host_time(stats,
                        ::bioseq::pipeline_stats::pipeline_stage::cigar_job_generation,
                        gpu_cigar_now_ns() - cigar_prepare_begin_ns);

    if (dispatch_params.do_cigar && !cigar_jobs.empty()) {
        const bioseq::gpu::vulkan::gpu_alignment_cigar_params cigar_params{
            .alphabet_size = static_cast<std::uint32_t>(
                compact_sequence_range<p_sequence>::traits::alphabet_size_n),
            .max_band_width = bioseq::gpu::vulkan::k_gpu_alignment_extension_max_band_width,
            .tile_query = bioseq::gpu::vulkan::k_gpu_alignment_cigar_traceback_tile_len,
            .tile_target = bioseq::gpu::vulkan::k_gpu_alignment_cigar_traceback_tile_len,
            .match_score = dispatch_params.match_score,
            .mismatch_penalty = dispatch_params.mismatch_pen,
            .gap_open_short = dispatch_params.gap_open1,
            .gap_extend_short = dispatch_params.gap_ext1,
            .gap_open_long = dispatch_params.gap_open2,
            .gap_extend_long = dispatch_params.gap_ext2,
            .z_drop = dispatch_params.zdrop,
            .end_bonus = std::max<std::int32_t>(0, dispatch_params.end_bonus),
            .flags = 0,
            .reserved = 0,
        };
        const auto cigar_run = runtime.run_alignment_simple_cigar_roundtrip(
            std::span<const std::uint32_t>{cigar_query_symbols.data(), cigar_query_symbols.size()},
            std::span<const std::uint32_t>{cigar_target_symbols.data(),
                                           cigar_target_symbols.size()},
            std::span<const bioseq::gpu::vulkan::gpu_alignment_cigar_job_wire>{cigar_jobs.data(),
                                                                               cigar_jobs.size()},
            cigar_params,
            packed_cigar_capacity);
        add_operational_counter(stats, stats.operational.gpu_extend_dispatches);
        if (cigar_run.results.size() != pending_cigar_matches.size()) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "GPU CIGAR extension result count mismatch");
        }
        for (std::size_t match_index = 0; match_index < pending_cigar_matches.size();
             ++match_index) {
            const auto& cigar_result = cigar_run.results[match_index];
            const auto& cigar_job = cigar_jobs[match_index];
            if (cigar_result.cigar_length == 0u) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "GPU CIGAR extension returned empty CIGAR");
            }
            const std::uint64_t cigar_end = static_cast<std::uint64_t>(cigar_result.cigar_offset) +
                                            static_cast<std::uint64_t>(cigar_result.cigar_length);
            if (cigar_end > cigar_run.packed_cigar.size()) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "GPU CIGAR extension returned invalid CIGAR slice");
            }

            gpu_cigar_pending_match& pending = pending_cigar_matches[match_index];
            match_result& match = pending.match;
            match.dp_score = cigar_result.score;
            match.dp_max_score = cigar_result.max_score;
            match.query_start = cigar_result.query_start;
            match.query_end = cigar_result.query_end;
            match.target_start = cigar_result.target_start;
            match.target_end = cigar_result.target_end;
            match.has_cigar = true;
            match.cigar.clear();

            const std::span<const std::uint32_t> packed{cigar_run.packed_cigar.data() +
                                                            cigar_result.cigar_offset,
                                                        cigar_result.cigar_length};
            sequence_size_t block_length = 0;
            sequence_size_t num_matches = 0;
            std::uint32_t query_cursor = 0;
            std::uint32_t target_cursor = 0;
            for (std::uint32_t entry : packed) {
                const std::uint32_t opcode = entry & 0xfU;
                const std::uint32_t operation_length = entry >> 4;
                if (opcode == cigar_match_opcode || opcode == cigar_equal_opcode ||
                    opcode == cigar_mismatch_opcode) {
                    block_length += operation_length;
                    const std::uint32_t query_base = cigar_job.query_symbol_offset + query_cursor;
                    const std::uint32_t target_base =
                        cigar_job.target_symbol_offset + target_cursor;
                    for (std::uint32_t symbol_offset = 0; symbol_offset < operation_length;
                         ++symbol_offset) {
                        if (cigar_query_symbols[query_base + symbol_offset] ==
                            cigar_target_symbols[target_base + symbol_offset]) {
                            ++num_matches;
                        }
                    }
                    query_cursor += operation_length;
                    target_cursor += operation_length;
                } else if (opcode == cigar_insertion_opcode) {
                    block_length += operation_length;
                    query_cursor += operation_length;
                } else if (opcode == cigar_deletion_opcode) {
                    block_length += operation_length;
                    target_cursor += operation_length;
                }
            }
            match.block_length = block_length;
            match.num_matches = num_matches;
            append_packed_cigar_slice(match.cigar, packed);
            results[pending.query_id].push_back(std::move(match));
        }
        add_operational_counter(stats,
                                stats.operational.host_cigar_construction_calls,
                                static_cast<std::uint64_t>(pending_cigar_matches.size()));
    }
    record_gpu_batch_outcomes();
    return;
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
