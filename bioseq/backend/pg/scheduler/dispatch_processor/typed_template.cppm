/**
 * @file
 * @brief Generate sequence dispatch functions for each alphabet and storage width.
 */
// Code generation reads these attributes and emits the dispatch variants.
// This file is generator input and is not compiled as a module itself.
module;

extern "C" {
#include "postgres.h"
#include "access/attnum.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

// Include standard headers herc so code generation can scan this file
// without depending on the compiler's prebuilt standard-library module.
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

export module bioseq.codegen.inputs.dispatch_processor_typed;

import bioseq.postgres.scheduler.dispatch_processor;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.row_adapters;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.source_provider_adapters;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.scanner_row;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.pg_alignment_errors;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.policy_stats;
import bioseq.postgres.scheduler.dispatch_processor.checked_alignment_calls;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.resident_target_materialization;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.generic_rows;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.cpu_batch_route;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.root_coordination;
import bioseq.postgres.scheduler.dispatch_processor.source_provider_promotion;
import bioseq.postgres.scheduler.dispatch_processor.fragment_storage_opener;
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
import bioseq.postgres.storage.indexing.residency;
import bioseq.postgres.storage.indexing.sorted_runs;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.chaining.vulkan;
import bioseq.sequences.cigar;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.pipeline.stats;
import bioseq.gpu.vulkan.runtime;
import bioseq.postgres.types.compact_sequence;
import bioseq.instrumentation.clock;
import bioseq.instrumentation.configuration;
import bioseq.instrumentation.descriptor;
import bioseq.instrumentation.profiler_state;
import bioseq.instrumentation.region;
import bioseq.instrumentation.runtime;
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

/*
 * The generated entry points choose  the typed scan and alignment functions.
 * The active read policy selects the MPF or MPS index format. Stored-index
 * reuse also depends on the statement's read requirement. Missing indexes can
 * require recomputation or an error, depending on that requirement.
 *
 * Saving recomputed indexes is a separate decision governed by the asynchronous
 * persistence settings and enabled insert policies. See process_scanner_pg_row_typed
 * for the row-level read and recompute paths.
 */
namespace bioseq::simd {
namespace typed_dispatch {

using alphabet
    [[bioseq::template_flatten(
        "identity", "alphabet",
        "dna4", "::seq::dna4",
        "dna5", "::seq::dna5")]]
    = ::seq::dna4;

static constexpr std::size_t storage_width
    [[bioseq::template_flatten(
        "identity", "width",
        "w1", "1",
        "w2", "2",
        "w4", "4",
        "w8", "8")]]
    = 1;

static constexpr std::uint8_t compile_shard
    [[bioseq::template_flatten(
        "shard", "shape",
        "root", "0",
        "scanner_pg", "1",
        "inmemory", "2",
        "inmemory_standard", "3",
        "inmemory_segmented", "4",
        "inmemory_chunked", "5",
        "inmemory_chunked_segmented", "6")]]
    = 0;

namespace {

// Share the typed helpers across generated dispatch variants.
using namespace ::bioseq::simd::typed_dispatch::typed_support_detail;

[[bioseq::template_flatten_when("shape", "root")]]
/// Check that both locators identify the same  target before sharing it
/// across worker threads. This check makes no PostgreSQL calls.
[[nodiscard]] constexpr bool same_dispatch_target_locator(
        const bioseq::storage::sequence_locator& left,
        const bioseq::storage::sequence_locator& right) noexcept {
    return typed_support_detail::same_dispatch_target_locator(left, right);
}

[[bioseq::template_flatten_when("shape", "root")]]
/// Check the stored hash-major reader and its target identity, before
/// worker threads borrow it
[[nodiscard]] inline bool hash_major_parallel_reader_identity_valid(
        const batch_processor::cpu_prepared_index_group& group,
        const std::uint64_t target_bp_size) noexcept {
    return typed_support_detail::hash_major_parallel_reader_identity_valid(
        group, target_bp_size);
}

[[bioseq::template_flatten_when("shape", "root")]]
/// Check the grouped-SoA fragment reader and its target identity before
/// worker threads borrow its fragment plans.
[[nodiscard]] inline bool fragment_parallel_reader_identity_valid(
        const batch_processor::cpu_prepared_index_group& group,
        const std::uint64_t target_bp_size) noexcept {
    return typed_support_detail::fragment_parallel_reader_identity_valid(
        group, target_bp_size);
}

[[bioseq::template_flatten_when("shape", "root")]]
/// Attach the saved query-sketch counters after the wxrker returns.
/// Reject results that already contain query counters to avoid counting twice.
inline void attach_retained_hash_major_query_stats(
        match_result_collection& collection,
        const pipe::minimizer_range_execution_stats& stats) {
    typed_support_detail::attach_retained_hash_major_query_stats(
        collection, stats);
}

/**
 * Open a lazy sequence view through the scan's source provider  and pass it to
 * the callback. The read session keeps the view alive until the callback and
 * its worker tasks have finished.
 */
template <class p_alphabet, class p_function>
[[bioseq::template_flatten_when("shape", "root")]]
inline void with_source_provider_pg_locator(
        batch_processor::scanner_pg_context& context,
        const bioseq::storage::sequence_locator& locator,
        const AttrNumber column_attno,
        const std::uint8_t alphabet_code,
        const std::uint8_t storage_unit_byte_width,
        const std::uint16_t last_padding_letters,
        pg_sequence_read_session* active_read_session,
        p_function&& callback) {
    typed_support_detail::with_source_provider_pg_locator<p_alphabet>(
        context, locator, column_attno, alphabet_code,
        storage_unit_byte_width, last_padding_letters,
        active_read_session, std::forward<p_function>(callback));
}

template <class p_alphabet, class p_function>
[[bioseq::template_flatten_when("shape", "root")]]
inline void with_pg_sequence_recipe(
        batch_processor::scanner_pg_context& context,
        const Datum recipe,
        const pg_sequence_descriptor& descriptor,
        const std::uint8_t alphabet_code,
        const std::uint8_t storage_unit_byte_width,
        p_function&& callback) {
    typed_support_detail::with_pg_sequence_recipe<p_alphabet>(
        context, recipe, descriptor, alphabet_code, storage_unit_byte_width,
        std::forward<p_function>(callback));
}

template <class p_sequence, class p_function>
[[bioseq::template_flatten_when("shape", "root", "scanner_pg")]]
inline void with_source_provider_pg_target(
        batch_processor::scanner_pg_context& context,
        const bioseq::storage::sequence_locator& locator,
        const std::uint8_t alphabet_code,
        const std::uint8_t storage_unit_byte_width,
        const std::uint16_t last_padding_letters,
        pg_sequence_read_session* active_read_session,
        p_function&& callback) {
    typed_support_detail::with_source_provider_pg_target<p_sequence>(
        context, locator, alphabet_code, storage_unit_byte_width,
        last_padding_letters, active_read_session,
        std::forward<p_function>(callback));
}

// Compile the PostgreSQL scanner path separately to keep GCC's function
// analysis manageable.
template <class p_sequence>
[[bioseq::template_flatten_when("shape", "scanner_pg")]]
[[nodiscard]] match_result_collection process_scanner_pg_row_typed(
    bioseq::storage::lazy_sequence_view<
        typename compact_sequence_range<p_sequence>::traits::alphabet_type> query_view,
    std::optional<bioseq::storage::lazy_sequence_view<
        typename compact_sequence_range<p_sequence>::traits::alphabet_type>>
        target_recipe_view,
    const bioseq::pg_types::seq* target_hdr,
    const std::size_t i,
    const sequence_size_t target_bp_size,
    const std::size_t min_bp,
    pipe::statement_execution_plan_owner execution_plan,
    const bool per_row_query_mode,
    std::shared_ptr<const std::vector<minimap_sketch_data>>& cached_query_minimizers,
    std::optional<pipe::minimizer_range_execution_stats>&
        cached_query_minimizer_stats,
    bool& consumed_shared_cached_query_stats,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    batch_processor::scanner_pg_context* scanner_pg_ctx,
    bioseq::parallelism::global_pool& pool,
    pg_sequence_read_session* active_read_session)
{
    auto with_pg_target_fn = []<class p_function>(
                                 batch_processor::scanner_pg_context& context,
                                 const bioseq::storage::sequence_locator& locator,
                                 const auto alphabet_code,
                                 const auto storage_unit_byte_width,
                                 const auto last_padding_letters,
                                 pg_sequence_read_session* read_session,
                                 p_function&& callback) {
        with_source_provider_pg_target<p_sequence>(
            context,
            locator,
            alphabet_code,
            storage_unit_byte_width,
            last_padding_letters,
            read_session,
            static_cast<p_function&&>(callback));
    };
    return typed_support_detail::process_scanner_pg_row_typed<p_sequence>(
        std::move(query_view), std::move(target_recipe_view), target_hdr, i,
        target_bp_size, min_bp, execution_plan, per_row_query_mode,
        cached_query_minimizers, cached_query_minimizer_stats,
        consumed_shared_cached_query_stats, alphabet_code,
        storage_unit_byte_width, scanner_pg_ctx, pool, active_read_session,
        typed_support_detail::open_typed_dispatch_fragment_storage,
        with_pg_target_fn);
}

template <class p_sequence>
[[bioseq::template_flatten_when("shape", "inmemory_standard")]]
[[nodiscard]] match_result_collection process_inmemory_standard_row_typed(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    pipe::statement_execution_plan_owner execution_plan)
{
    compact_sequence_range<p_sequence> query_seq{*row_query_hdr};
    compact_sequence_range<p_sequence> target_seq{*target_hdr};
    const auto lazy_query_view =
        bioseq::storage::make_lazy_sequence_view(query_seq);
    const auto lazy_target_view =
        bioseq::storage::make_lazy_sequence_view(target_seq);
    return minimap_port_raw_pg_checked(
        lazy_query_view, lazy_target_view, execution_plan);
}

template <class p_sequence>
[[bioseq::template_flatten_when("shape", "inmemory_segmented")]]
[[nodiscard]] match_result_collection process_inmemory_segmented_row_typed(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    pipe::statement_execution_plan_owner execution_plan)
{
    compact_sequence_range<p_sequence> query_seq{*row_query_hdr};
    compact_sequence_range<p_sequence> target_seq{*target_hdr};
    const auto lazy_query_view =
        bioseq::storage::make_lazy_sequence_view(query_seq);
    const auto lazy_target_view =
        bioseq::storage::make_lazy_sequence_view(target_seq);
    return minimap_port_segmented_pg_checked(
        lazy_query_view, lazy_target_view, execution_plan);
}

template <class p_sequence>
[[bioseq::template_flatten_when("shape", "inmemory_chunked")]]
[[nodiscard]] match_result_collection process_inmemory_chunked_row_typed(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    pipe::statement_execution_plan_owner execution_plan)
{
    compact_sequence_range<p_sequence> query_seq{*row_query_hdr};
    compact_sequence_range<p_sequence> target_seq{*target_hdr};
    match_result_collection collection;
    bioseq::storage::with_chunked_lazy_sequence(
        query_seq, target_seq,
        [&collection, execution_plan](auto query_view, auto target_view) {
            collection = minimap_port_raw_pg_checked(
                query_view, target_view, execution_plan);
        });
    return collection;
}

template <class p_sequence>
[[bioseq::template_flatten_when("shape", "inmemory_chunked_segmented")]]
[[nodiscard]] match_result_collection process_inmemory_chunked_segmented_row_typed(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    pipe::statement_execution_plan_owner execution_plan)
{
    compact_sequence_range<p_sequence> query_seq{*row_query_hdr};
    compact_sequence_range<p_sequence> target_seq{*target_hdr};
    match_result_collection collection;
    bioseq::storage::with_chunked_lazy_sequence(
        query_seq, target_seq,
        [&collection, execution_plan](auto query_view, auto target_view) {
            collection = minimap_port_segmented_pg_checked(
                query_view, target_view, execution_plan);
        });
    return collection;
}

template <class p_sequence>
[[bioseq::template_flatten_when("shape", "root", "inmemory")]]
[[nodiscard]] match_result_collection process_inmemory_row_typed(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const sequence_size_t target_bp_size,
    pipe::statement_execution_plan_owner execution_plan,
    const bool chunked_scanner_forced)
{
    const bool segmented = target_bp_size > postgres_segmentation_threshold_bp;
    if (chunked_scanner_forced) {
        return segmented
            ? process_inmemory_chunked_segmented_row(
                row_query_hdr, target_hdr, execution_plan)
            : process_inmemory_chunked_row(
                row_query_hdr, target_hdr, execution_plan);
    }
    return segmented
        ? process_inmemory_segmented_row(
            row_query_hdr, target_hdr, execution_plan)
        : process_inmemory_standard_row(
            row_query_hdr, target_hdr, execution_plan);
}

template <class p_sequence>
[[bioseq::template_flatten_when("shape", "root")]]
[[nodiscard]] auto process_sequences_typed(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>*
        per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>&
        target_hdrs,
    const std::array<int32_t, dispatch_batch_capacity>& query_ids,
    pipe::statement_execution_plan_owner execution_plan,
    const batch_invocation_role invocation_role,
    const std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
    batch_processor::scanner_pg_context* scanner_pg_ctx)
    -> match_result_map
{
    using gpu_row_alphabet = typename compact_sequence_range<p_sequence>::traits::alphabet_type;
    using gpu_row_view = bioseq::storage::lazy_sequence_view<gpu_row_alphabet>;
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
            source_datum = typed_support_detail::promote_source_provider_pg_locator(
                scanner_pg_ctx->source_provider,
                scanner_pg_ctx->parent_mctx,
                *locator,
                static_cast<AttrNumber>(query_operand ? scanner_pg_ctx->mmsearch_query_attno
                                                      : scanner_pg_ctx->mmsearch_seq_attno),
                source_alphabet,
                source_width,
                static_cast<std::uint16_t>(padding));
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
        auto* source_owner = bioseq::postgres::support::invoke_pg_as_cpp(
            [scanner_pg_ctx, source_datum, descriptor] {
                return ::make_pg_sequence_call_owner<gpu_row_alphabet>(
                    *scanner_pg_ctx->read_session, source_datum, descriptor);
            });
        const auto* observer =
            query_operand ? std::addressof(scanner_pg_ctx->mmsearch_query_payload_observer)
                          : std::addressof(scanner_pg_ctx->mmsearch_target_payload_observer);
        return source_owner->view().with_payload_allocation_observer(observer);
    };
    auto scanner_row = []<class... p_args>(p_args&&... args)
        -> decltype(auto) {
        return process_scanner_pg_row(
            std::forward<p_args>(args)...);
    };
    auto inmemory_row = []<class... p_args>(p_args&&... args)
        -> decltype(auto) {
        return process_inmemory_row_typed<p_sequence>(
            std::forward<p_args>(args)...);
    };
    const auto materialize_target = [](auto target_view,
                                       const std::uint8_t requested_width,
                                       const std::uint64_t sequence_rowno) {
        return typed_support_detail::try_materialize_target_for_segment_workers(
            target_view, requested_width, sequence_rowno);
    };
    const auto same_target_locator = [](const auto& left, const auto& right) noexcept {
        return same_dispatch_target_locator(left, right);
    };
    const auto hash_major_identity_valid = [](const auto& group,
                                               const std::uint64_t target_bp_size) noexcept {
        return hash_major_parallel_reader_identity_valid(group, target_bp_size);
    };
    const auto fragment_identity_valid = [](const auto& group,
                                             const std::uint64_t target_bp_size) noexcept {
        return fragment_parallel_reader_identity_valid(group, target_bp_size);
    };
    const auto with_pg_target = [](auto& context,
                                   const auto& locator,
                                   const std::uint8_t target_alphabet_code,
                                   const std::uint8_t target_storage_unit_byte_width,
                                   const std::uint16_t last_padding_letters,
                                   pg_sequence_read_session* active_read_session,
                                   auto&& callback) {
        with_source_provider_pg_target<p_sequence>(
            context,
            locator,
            target_alphabet_code,
            target_storage_unit_byte_width,
            last_padding_letters,
            active_read_session,
            std::forward<decltype(callback)>(callback));
    };
    const auto attach_query_stats = [](auto& collection, const auto& execution_stats) {
        attach_retained_hash_major_query_stats(collection, execution_stats);
    };
    const auto with_pg_sequence_recipe_fn = []<class p_alphabet, class... p_args>(
                                                p_args&&... args) -> decltype(auto) {
        return with_pg_sequence_recipe<p_alphabet>(std::forward<p_args>(args)...);
    };
    const auto with_source_provider_pg_locator_fn = []<class p_alphabet, class... p_args>(
                                                      p_args&&... args) -> decltype(auto) {
        return with_source_provider_pg_locator<p_alphabet>(std::forward<p_args>(args)...);
    };
    const auto process_generic_rows_callback = [&with_pg_sequence_recipe_fn,
                                                &with_source_provider_pg_locator_fn]
                                               <class... p_args>(p_args&&... args)
        -> decltype(auto) {
        return typed_support_detail::process_generic_rows<p_sequence>(
            std::forward<p_args>(args)...,
            with_pg_sequence_recipe_fn,
            with_source_provider_pg_locator_fn);
    };
    auto cpu_route = [&scanner_row,
                      &inmemory_row,
                      &materialize_target,
                      &same_target_locator,
                      &hash_major_identity_valid,
                      &fragment_identity_valid,
                      &with_pg_target,
                      &attach_query_stats,
                      &process_generic_rows_callback]<class... p_args>(p_args&&... args)
        -> decltype(auto) {
        return typed_support_detail::process_cpu_batch_route<p_sequence>(
            std::forward<p_args>(args)...,
            scanner_row,
            inmemory_row,
            materialize_target,
            same_target_locator,
            hash_major_identity_valid,
            fragment_identity_valid,
            with_pg_target,
            attach_query_stats,
            process_generic_rows_callback);
    };
    return typed_support_detail::process_sequences_typed_root<p_sequence>(
        batch_size,
        alphabet_code,
        storage_unit_byte_width,
        query_hdr,
        per_row_query_hdrs,
        target_hdrs,
        query_ids,
        execution_plan,
        invocation_role == batch_invocation_role::primary,
        broadcast_query_ordinal,
        stats,
        scanner_pg_ctx,
        source_view_for_row,
        same_target_locator,
        cpu_route);
}

} // namespace

[[bioseq::template_flatten_when("shape", "scanner_pg")]]
[[bioseq::template_flatten_entry("scanner_pg")]]
[[nodiscard]] match_result_collection
process_scanner_pg_row(
    bioseq::storage::lazy_sequence_view<
        alphabet> query_view,
    std::optional<bioseq::storage::lazy_sequence_view<
        alphabet>> target_recipe_view,
    const bioseq::pg_types::seq* target_hdr,
    const std::size_t i,
    const sequence_size_t target_bp_size,
    const std::size_t min_bp,
    pipe::statement_execution_plan_owner execution_plan,
    const bool per_row_query_mode,
    std::shared_ptr<const std::vector<minimap_sketch_data>>& cached_query_minimizers,
    std::optional<pipe::minimizer_range_execution_stats>&
        cached_query_minimizer_stats,
    bool& consumed_shared_cached_query_stats,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    batch_processor::scanner_pg_context* scanner_pg_ctx,
    bioseq::parallelism::global_pool& pool,
    pg_sequence_read_session* active_read_session)
{
    using sequence_type = compact_sequence_stdc_instance<
        bioseq::pg_types::seq,
        alphabet,
        storage_width>;
    return process_scanner_pg_row_typed<sequence_type>(
        query_view,
        target_recipe_view,
        target_hdr,
        i,
        target_bp_size,
        min_bp,
        execution_plan,
        per_row_query_mode,
        cached_query_minimizers,
        cached_query_minimizer_stats,
        consumed_shared_cached_query_stats,
        alphabet_code,
        storage_unit_byte_width,
        scanner_pg_ctx,
        pool,
        active_read_session);
}

[[bioseq::template_flatten_when("shape", "inmemory")]]
[[bioseq::template_flatten_entry("inmemory")]]
[[nodiscard]] match_result_collection
process_inmemory_row(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const sequence_size_t target_bp_size,
    pipe::statement_execution_plan_owner execution_plan,
    const bool chunked_scanner_forced)
{
    using sequence_type = compact_sequence_stdc_instance<
        bioseq::pg_types::seq,
        alphabet,
        storage_width>;
    return process_inmemory_row_typed<sequence_type>(
        row_query_hdr,
        target_hdr,
        target_bp_size,
        execution_plan,
        chunked_scanner_forced);
}

[[bioseq::template_flatten_when("shape", "inmemory_standard")]]
[[bioseq::template_flatten_entry("inmemory_standard")]]
[[nodiscard]] match_result_collection
process_inmemory_standard_row(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    pipe::statement_execution_plan_owner execution_plan)
{
    using sequence_type = compact_sequence_stdc_instance<
        bioseq::pg_types::seq,
        alphabet,
        storage_width>;
    return process_inmemory_standard_row_typed<sequence_type>(
        row_query_hdr,
        target_hdr,
        execution_plan);
}

[[bioseq::template_flatten_when("shape", "inmemory_segmented")]]
[[bioseq::template_flatten_entry("inmemory_segmented")]]
[[nodiscard]] match_result_collection
process_inmemory_segmented_row(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    pipe::statement_execution_plan_owner execution_plan)
{
    using sequence_type = compact_sequence_stdc_instance<
        bioseq::pg_types::seq,
        alphabet,
        storage_width>;
    return process_inmemory_segmented_row_typed<sequence_type>(
        row_query_hdr,
        target_hdr,
        execution_plan);
}

[[bioseq::template_flatten_when("shape", "inmemory_chunked")]]
[[bioseq::template_flatten_entry("inmemory_chunked")]]
[[nodiscard]] match_result_collection
process_inmemory_chunked_row(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    pipe::statement_execution_plan_owner execution_plan)
{
    using sequence_type = compact_sequence_stdc_instance<
        bioseq::pg_types::seq,
        alphabet,
        storage_width>;
    return process_inmemory_chunked_row_typed<sequence_type>(
        row_query_hdr,
        target_hdr,
        execution_plan);
}

[[bioseq::template_flatten_when("shape", "inmemory_chunked_segmented")]]
[[bioseq::template_flatten_entry("inmemory_chunked_segmented")]]
[[nodiscard]] match_result_collection
process_inmemory_chunked_segmented_row(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    pipe::statement_execution_plan_owner execution_plan)
{
    using sequence_type = compact_sequence_stdc_instance<
        bioseq::pg_types::seq,
        alphabet,
        storage_width>;
    return process_inmemory_chunked_segmented_row_typed<sequence_type>(
        row_query_hdr,
        target_hdr,
        execution_plan);
}

[[bioseq::template_flatten_when("shape", "root")]]
[[bioseq::template_flatten_entry("root")]]
[[nodiscard]] match_result_map process_sequences(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>& target_hdrs,
    const std::array<int32_t, dispatch_batch_capacity>& query_ids,
    pipe::statement_execution_plan_owner execution_plan,
    const batch_invocation_role invocation_role,
    const std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
    batch_processor::scanner_pg_context* scanner_pg_ctx)
{
    using sequence_type = compact_sequence_stdc_instance<
        bioseq::pg_types::seq,
        alphabet,
        storage_width>;
    if (!execution_plan) {
        throw std::logic_error("generated root requires its immutable statement plan");
    }
    const auto& base_params = execution_plan->frozen_parameters;
    const auto& typmod_operations =
        cpu_preparation_detail::typmod_ops<alphabet, storage_width>();
    if (!typmod_operations.matches(
            alphabet_code, storage_unit_byte_width)) {
        throw std::logic_error{
            "generated scheduler typmod operations disagree with the selected root"};
    }
    if (scanner_pg_ctx != nullptr
        && scanner_pg_ctx->highway_preparation.valid()) {
        auto preparation = std::exchange(
            scanner_pg_ctx->highway_preparation,
            batch_processor::scanner_pg_context::highway_preparation_hook{});
        preparation.function(
            preparation.state,
            *scanner_pg_ctx,
            execution_plan,
            typmod_operations);
        if (!scanner_pg_ctx->cpu_prepared_target) {
            throw std::logic_error{
                "Highway preparation hook did not publish a CPU target"};
        }
    }
    if (scanner_pg_ctx != nullptr
        && scanner_pg_ctx->cpu_prepared_target
        && (!scanner_pg_ctx->cpu_prepared_target->unions_ready
            || !scanner_pg_ctx->cpu_prepared_target
                ->highway_binding_matches(execution_plan))) {
        throw std::logic_error{
            "prepared CPU target disagrees with the selected Highway binding"};
    }

    const auto run_pipeline = [&alphabet_code,
                               execution_plan,
                               &batch_size,
                               &broadcast_query_ordinal,
                               &invocation_role,
                               &per_row_query_hdrs,
                               &query_hdr,
                               &query_ids,
                               &scanner_pg_ctx,
                               &stats,
                               &storage_unit_byte_width,
                               &target_hdrs] {
        return process_sequences_typed<sequence_type>(
            batch_size,
            alphabet_code,
            storage_unit_byte_width,
            query_hdr,
            per_row_query_hdrs,
            target_hdrs,
            query_ids,
            execution_plan,
            invocation_role,
            broadcast_query_ordinal,
            stats,
            scanner_pg_ctx);
    };
    if (!base_params.enable_profiling) {
        return run_pipeline();
    }

    /* Profile batch execution after target preparation has finished.
     * The scan-end hook writes the accumulated summary. */
    using profiling_arguments = bioseq::instrumentation::production_profiling_configuration;
    using timing_arguments = bioseq::instrumentation::benchmark_profiling_configuration;
    auto& profiler_block = bioseq::instrumentation::persistent_profiler_state<profiling_arguments>();
    auto& timing_profiler_block =
        bioseq::instrumentation::persistent_profiler_state<timing_arguments>();
    bioseq::instrumentation::installed_profiler_scope<profiling_arguments> guard(profiler_block);
    bioseq::instrumentation::installed_profiler_scope<timing_arguments> timing_guard(
        timing_profiler_block);
    static const auto* batch_desc = bioseq::instrumentation::register_static_region(
        "batch_processor.process_sequences");
    [[maybe_unused]] auto batch_scope = bioseq::instrumentation::region<profiling_arguments>(batch_desc);
    return run_pipeline();
}

} // namespace typed_dispatch
} // namespace bioseq::simd
