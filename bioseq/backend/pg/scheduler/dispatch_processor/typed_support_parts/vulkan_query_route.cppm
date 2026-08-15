/**
 * @file typed_support_parts/vulkan_query_route.cppm
 * @brief Owns Vulkan query-source admission and device-only sketch preparation.
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

export module bioseq.postgres.scheduler.dispatch_processor.typed_support:vulkan_query_route;

import bioseq.postgres.scheduler.dispatch_processor;
import :policy_stats;
import :pg_alignment_errors;
import :transient_storage;
import :resident_targets;
import :row_adapters;
import :scanner_row;
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
 * Associates one generated query row with its device sketch record.
 */
struct gpu_batch_query_sidecar {
    std::size_t row_index = 0;
    sequence_size_t query_len = 0;
};

/**
 * Retains every query-side owner and device authority across the persisted or
 * resident Vulkan route selected by the root coordinator.
 */
template <class p_sequence> struct vulkan_query_batch_state {
    using minimizer_type = minimap_sketch_data;
    using gpu_row_alphabet = typename compact_sequence_range<p_sequence>::traits::alphabet_type;
    using gpu_row_view = bioseq::storage::lazy_sequence_view<gpu_row_alphabet>;
    using gpu_admitted_owner = bioseq::storage::admitted_packed_sequence<gpu_row_alphabet>;

    std::vector<gpu_admitted_owner> gpu_query_admitted_owners;
    std::array<std::optional<gpu_row_view>, dispatch_batch_capacity> gpu_query_recipe_views{};
    std::array<std::optional<gpu_row_view>, dispatch_batch_capacity> gpu_target_recipe_views{};
    std::array<std::optional<std::size_t>, dispatch_batch_capacity> gpu_query_resident{};
    std::size_t w{};
    std::size_t k{};
    std::shared_ptr<bioseq::gpu::vulkan::vulkan_batch_sketch_memory_retention>
        query_sketch_memory_retention;
    std::vector<bioseq::gpu::vulkan::gpu_seed_query_record_wire> query_records;
    bioseq::gpu::vulkan::vulkan_device_minimizer_span<minimizer_type> query_minimizer_device_span{};
    bioseq::gpu::vulkan::device_query_sketch_authorization query_sketch_authority{};
    std::vector<gpu_batch_query_sidecar> query_sidecars;
    std::vector<std::uint32_t> row_to_query_index;
    pipe::ordered_sketch_query_gate_storage query_gate;
    std::uint64_t gpu_query_sketch_host_ns{};
};

/**
 * Admits recipe/locator query inputs and produces the one device-only query
 * sketch state consumed by either Vulkan target route.
 */
template <class p_sequence>
void prepare_vulkan_query_route(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>* per_row_query_hdrs,
    const minimap2_parameters& dispatch_params,
    batch_processor::scanner_pg_context* scanner_pg_ctx,
    const bool per_row_query_mode,
    vulkan_query_batch_state<p_sequence>& state) {
    using gpu_row_alphabet = typename vulkan_query_batch_state<p_sequence>::gpu_row_alphabet;
    using gpu_row_view = typename vulkan_query_batch_state<p_sequence>::gpu_row_view;
    auto& gpu_query_admitted_owners = state.gpu_query_admitted_owners;
    gpu_query_admitted_owners.reserve(batch_size);
    auto& gpu_query_recipe_views = state.gpu_query_recipe_views;
    auto& gpu_target_recipe_views = state.gpu_target_recipe_views;
    auto& gpu_query_resident = state.gpu_query_resident;
    bool any_source_view = false;
    if (scanner_pg_ctx != nullptr) {
        for (std::size_t row = 0u; row < batch_size && !any_source_view; ++row) {
            any_source_view = scanner_pg_ctx->query_recipe_datums[row] != Datum{0} ||
                              scanner_pg_ctx->target_recipe_datums[row] != Datum{0} ||
                              scanner_pg_ctx->query_locators[row].has_value() ||
                              scanner_pg_ctx->target_locators[row].has_value();
        }
    }
    if (any_source_view) {
        if (scanner_pg_ctx == nullptr || scanner_pg_ctx->parent_mctx == nullptr ||
            scanner_pg_ctx->read_session == nullptr) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan recipe batch has no read-session arena");
        }
        auto source_view_for_row = [scanner_pg_ctx, alphabet_code, storage_unit_byte_width](
                                       const std::size_t row,
                                       const bool query_operand) -> std::optional<gpu_row_view> {
            const Datum recipe = query_operand ? scanner_pg_ctx->query_recipe_datums[row]
                                               : scanner_pg_ctx->target_recipe_datums[row];
            const auto& locator = query_operand ? scanner_pg_ctx->query_locators[row]
                                                : scanner_pg_ctx->target_locators[row];
            if (recipe != Datum{0} && locator.has_value()) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "Vulkan source operand overlaps recipe and locator ownership");
            }
            Datum source_datum = recipe;
            pg_sequence_descriptor descriptor =
                query_operand ? scanner_pg_ctx->query_recipe_descriptors[row]
                              : scanner_pg_ctx->target_recipe_descriptors[row];
            const std::uint8_t source_alphabet =
                query_operand ? scanner_pg_ctx->query_alphabet_code : alphabet_code;
            const std::uint8_t source_width = query_operand
                                                  ? scanner_pg_ctx->query_storage_unit_byte_width
                                                  : storage_unit_byte_width;
            if (locator.has_value()) {
                const std::size_t letters_per_unit = bioseq::storage::packed_letters_per_unit(
                    source_width, static_cast<std::uint8_t>(p_sequence::letter_bit_width));
                if (letters_per_unit == 0u) {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error(
                        "Vulkan locator has invalid packed geometry");
                }
                const std::uint64_t remainder = locator->logical_length % letters_per_unit;
                const std::uint64_t padding = remainder == 0u ? 0u : letters_per_unit - remainder;
                if (padding > (std::numeric_limits<std::uint16_t>::max)()) {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error(
                        "Vulkan locator padding exceeds ABI");
                }
                bioseq::pg_types::seq* stub =
                    bioseq::postgres::support::invoke_pg_as_cpp([scanner_pg_ctx,
                                                                 source_alphabet,
                                                                 source_width,
                                                                 padding,
                                                                 source_locator = *locator] {
                        return bioseq::storage::alloc_locator_stub_in(
                            scanner_pg_ctx->parent_mctx,
                            source_alphabet,
                            source_width,
                            static_cast<std::uint16_t>(padding),
                            source_locator.logical_length,
                            source_locator);
                    });
                source_datum = bioseq::storage::promote_pg_sequence_locator(
                    scanner_pg_ctx->source_provider,
                    static_cast<AttrNumber>(query_operand ? scanner_pg_ctx->mmsearch_query_attno
                                                          : scanner_pg_ctx->mmsearch_seq_attno),
                    PointerGetDatum(stub),
                    scanner_pg_ctx->parent_mctx);
                descriptor = bioseq::postgres::support::invoke_pg_as_cpp(
                    [source_datum] { return ::classify_and_describe_sequence(source_datum); });
                if (descriptor.bp_length != locator->logical_length) {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error(
                        "Vulkan locator promotion changed sequence length");
                }
            }
            if (source_datum == Datum{0}) {
                return std::nullopt;
            }
            const bool expanded = descriptor.kind == pg_sequence_datum_kind::expanded_read_only ||
                                  descriptor.kind == pg_sequence_datum_kind::expanded_read_write;
            if (!expanded || !descriptor.alphabet_bound ||
                descriptor.alphabet_code != source_alphabet ||
                descriptor.storage_unit_byte_width != source_width) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "Vulkan source facade metadata is invalid");
            }
            auto* owner = bioseq::postgres::support::invoke_pg_as_cpp(
                [scanner_pg_ctx, source_datum, descriptor] {
                    return ::make_pg_sequence_call_owner<gpu_row_alphabet>(
                        *scanner_pg_ctx->read_session, source_datum, descriptor);
                });
            const auto* observer =
                query_operand ? std::addressof(scanner_pg_ctx->mmsearch_query_payload_observer)
                              : std::addressof(scanner_pg_ctx->mmsearch_target_payload_observer);
            return owner->view().with_payload_allocation_observer(observer);
        };
        for (std::size_t row = 0u; row < batch_size; ++row) {
            const auto query_source = source_view_for_row(row, /*query_operand=*/true);
            if (query_source.has_value()) {
                const gpu_row_view original = *query_source;
                const auto access = bioseq::storage::plan_sequence_access(
                    original,
                    bioseq::storage::sequence_access_requirement::contiguous_resident,
                    0u,
                    original.bp_size(),
                    storage_unit_byte_width);
                const std::uint64_t admitted_bytes =
                    access.route == bioseq::storage::sequence_access_route::materialize_admitted
                        ? access.admission_bytes
                        : bioseq::storage::packed_sequence_admission_bytes<gpu_row_alphabet>(
                              original.bp_size(), storage_unit_byte_width);
                auto permit =
                    bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                        admitted_bytes);
                if (!permit.has_value()) {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error(
                        "Vulkan query recipe packed-region admission was rejected");
                }
                gpu_query_admitted_owners.push_back(bioseq::storage::materialize_packed_sequence(
                    original, 0u, original.bp_size(), storage_unit_byte_width, std::move(*permit)));
                gpu_query_resident[row] = gpu_query_admitted_owners.size() - 1u;
                gpu_query_recipe_views[row] = gpu_query_admitted_owners.back().view();
            }
            const auto target_source = source_view_for_row(row, /*query_operand=*/false);
            if (target_source.has_value()) {
                gpu_target_recipe_views[row] = *target_source;
            }
        }
    }
    state.w = static_cast<std::size_t>(dispatch_params.window_size);
    state.k = static_cast<std::size_t>(dispatch_params.kmer_size);
    const std::size_t w = state.w;
    const std::size_t k = state.k;
    // The resident query sketch remains device-only.  A fair
    // no-index target build is complete and query-independent;
    // host-materialising query hashes to filter that build
    // would create a direct-match accelerator, not an index.
    auto& query_sketch_memory_retention = state.query_sketch_memory_retention;
    auto& query_records = state.query_records;
    auto& query_minimizer_device_span = state.query_minimizer_device_span;
    auto& query_sketch_authority = state.query_sketch_authority;
    auto& query_sidecars = state.query_sidecars;
    auto& row_to_query_index = state.row_to_query_index;
    row_to_query_index.assign(batch_size, std::numeric_limits<std::uint32_t>::max());
    auto& query_gate = state.query_gate;
    std::vector<bioseq::gpu::vulkan::sequence_upload_view> batch_query_uploads;
    const auto gpu_batch_now_ns = []() -> std::uint64_t {
        return static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
    };
    const std::uint64_t gpu_query_sketch_begin_ns = gpu_batch_now_ns();

    query_sidecars.reserve(batch_size);
    batch_query_uploads.reserve(batch_size);
    for (std::size_t i = 0; i < batch_size; ++i) {
        const bioseq::pg_types::seq* row_query_hdr =
            per_row_query_mode ? (*per_row_query_hdrs)[i] : query_hdr;
        const bool recipe_query = gpu_query_resident[i].has_value();
        if (row_query_hdr != nullptr && recipe_query) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan query recipe overlaps a flat query");
        }
        if (row_query_hdr == nullptr && !recipe_query) {
            continue;
        }
        if (row_query_hdr != nullptr &&
            (row_query_hdr->alphabet_code != alphabet_code ||
             row_query_hdr->storage_unit_byte_width != storage_unit_byte_width)) {
            continue;
        }
        const std::uint32_t query_index = static_cast<std::uint32_t>(batch_query_uploads.size());
        sequence_size_t query_length = 0u;
        if (recipe_query) {
            const auto owner_index = *gpu_query_resident[i];
            if (owner_index >= gpu_query_admitted_owners.size()) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "Vulkan query recipe owner index is invalid");
            }
            const auto& admitted = gpu_query_admitted_owners[owner_index];
            batch_query_uploads.push_back(
                bioseq::gpu::vulkan::make_sequence_upload_view<gpu_row_alphabet>(admitted));
            query_length = admitted.descriptor().logical_length;
        } else {
            compact_sequence_range<p_sequence> query_seq{*row_query_hdr};
            batch_query_uploads.push_back(
                bioseq::gpu::vulkan::make_sequence_upload_view(query_seq));
            query_length = query_seq.bp_size();
        }
        query_sidecars.push_back(gpu_batch_query_sidecar{
            .row_index = i,
            .query_len = query_length,
        });
        row_to_query_index[i] = query_index;
    }
    if (!batch_query_uploads.empty()) {
        const auto rank_lut =
            bioseq::gpu::vulkan::make_vulkan_rank_lut<compact_sequence_range<p_sequence>>();
        auto gpu_batch_sketch =
            bioseq::gpu::vulkan::collect_minimap_sketch_data_vulkan_upload_batch_result(
                std::span<const bioseq::gpu::vulkan::sequence_upload_view>{
                    batch_query_uploads.data(), batch_query_uploads.size()},
                rank_lut,
                w,
                k,
                bioseq::gpu::vulkan::vulkan_sketch_host_materialization::device_only);
        if (!gpu_batch_sketch) {
            pipe::throw_vulkan_declined("Vulkan batched query minimizer sketch");
        }
        query_records = std::move(gpu_batch_sketch->query_records);
        query_sketch_memory_retention = gpu_batch_sketch->memory_retention;
        query_minimizer_device_span = gpu_batch_sketch->device_span();
        query_sketch_authority = std::move(gpu_batch_sketch->query_authority);
    }
    state.gpu_query_sketch_host_ns = gpu_batch_now_ns() - gpu_query_sketch_begin_ns;
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
