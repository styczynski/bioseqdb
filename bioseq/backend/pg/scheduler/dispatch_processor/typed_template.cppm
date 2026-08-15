/**
 * @file
 * @brief Implements typed sequence dispatch for generated namespaces.
 */
// Attribute-driven namespace-materialization input only. This scan-only module
// supplies canonical source geometry; it is never attached to a compiler target.
module;

#include <libassert/assert.hpp>
#include <bioseq/pg_compat.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

export module bioseq.codegen.inputs.dispatch_processor_typed;

import bioseq.postgres.scheduler.dispatch_processor;
import bioseq.postgres.scheduler.dispatch_processor.typed_support;
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

namespace bioseq::simd::cpu_preparation_detail {

template <class p_alphabet, std::size_t p_width>
[[nodiscard]] const batch_processor::cpu_preparation_typmod_operations&
typmod_ops() noexcept;

} // namespace bioseq::simd::cpu_preparation_detail

/*-------------------------------------------------------------------------
 *
 * scheduler/dispatch_processor/index.cppm
 *
 * Heavy minimap/pipe implementation for batch_processor. Kept out of the
 * dispatch_buffer interface so edits to tuple-buffer or admin code do not
 * force that translation unit to reinstantiate the full supported alphabet x
 * storage-width minimap pipeline.
 *
 * Read/recompute/persist contract for minimizer indexes
 * -----------------------------------------------------
 *
 * There are two separate policy concepts in this file:
 *
 * 1. The active minimizer policy (`scanner_pg_context::active_index`).
 *    - Resolved once for the target column when a user relation is known.
 *      `bioseq.index.sequence_read.enabled` controls whether stored chunks are read;
 *      recompute paths still use this policy to choose transient
 *      minimizer-fragment vs minimizer-sequence storage.
 *    - Default when no enabled active row is found is
 *      `minimizers_per_fragment`.
 *    - The SQL schema has a unique partial index on
 *      `(user_relid, main_relfilenode, column_attno) WHERE is_active_read`, and
 *      `bioseq.set_active_read_index()` clears old active rows before setting
 *      the new one. Therefore more than one active read index is not a normal
 *      state. If an old/corrupt catalog ever exposes more than one, the resolver
 *      uses `LIMIT 1` with no ordering; treat that as unsupported.
 *
 * 2. The deferred recompute persistence choice.
 *    - Used only when `can_defer_recompute_minimizer_persist()` is true:
 *      `bioseq.index.recompute_async_persistence.enabled = on`,
 *      `bioseq.index.async_persistence.enabled = on`, a user relation is known, and the
 *      effective minimizer parameters are the persisted-index pair `k=15,w=10`.
 *    - It ignores `active_index` and scans `k_minimizer_index_names` in persistence
 *      preference order: `minimizers_per_fragment`, then
 *      `minimizers_per_sequence`.
 *    - If both minimizer indexes are enabled for UPDATE_ON_INSERT,
 *      `minimizers_per_fragment` wins. If neither is enabled, no deferred
 *      minimizer persist target is selected.
 *
 * GUC meanings in this dispatch path:
 *
 * - `bioseq.index.insert_maintenance.enabled`: write/load-time policy gate. When off,
 *   INSERT/load does not build UPDATE_ON_INSERT indexes. It does not by itself
 *   change the read path except by making stored indexes absent.
 *
 * - `bioseq.index.sequence_read.enabled`: read-time gate for stored-index reuse. When
 *   on, each row tries to assemble the active stored index if chunks exist.
 *   When off, no stored minimizer index is read even if chunks are present;
 *   rows go to recompute/transient paths that still honor the active
 *   minimizer-fragment/minimizer-sequence policy shape.
 *
 * - `bioseq.index.async_persistence.enabled`: global async persistence gate. If off, async
 *   staging functions refuse to enqueue/write opportunistically built indexes.
 *   It does not pick an index by itself.
 *
 * - `bioseq.index.recompute_async_persistence.enabled`: no-index recompute gate. It allows a
 *   recompute path to consult UPDATE_ON_INSERT minimizer policy and stage or
 *   otherwise handle a deferred minimizer target, but only together with
 *   `bioseq.index.async_persistence.enabled` and only for persisted parameters `k=15,w=10`.
 *
 * - `bioseq.index.memory_cap_mb`: memory-admission cap for resident targets,
 *   assembled indexes, sorted runs, and opportunistic RAM-first builds. `0`
 *   means use the live process/system memory budget. A larger or smaller cap
 *   can admit or reject resident/parallel paths, but it does not force
 *   persistence and it does not choose the active index.
 *
 * - `bioseq.scheduler.pipeline.mode` and
 *   `bioseq.scheduler.backend.max_concurrency`: enable task-parallel segment work when
 *   mode is `task_parallel` and effective concurrency is greater than one.
 *   These affect speed and resident materialization attempts, not the logical
 *   persistence policy.
 *
 * Stored-index present:
 *
 * - Active read index = `minimizers_per_fragment`.
 *   If the row has fragment-index chunks, assemble the required full/segment
 *   index from stored chunks and search with it. No new persistence is staged.
 *   If chunks are missing, fall through to recompute.
 *
 * - Active read index = `minimizers_per_sequence`.
 *   If the row has minimizer-sequence chunks, assemble a query-specific index
 *   from the stored minimizer-sequence representation and search with it.
 *   Logical-fragment-backed minimizer-sequence chunks remain supported. No new
 *   persistence is staged. If chunks are
 *   missing, fall through to recompute.
 *
 * - No active read row.
 *   The resolver defaults to `minimizers_per_fragment` when reads are enabled.
 *   If `bioseq.index.sequence_read.enabled` is off, no stored index is read; the
 *   resolved policy still chooses the transient recompute shape.
 *
 * Stored-index missing or reads disabled:
 *
 * - Active read index = `minimizers_per_fragment`.
 *   Recompute the search index. If RAM-first opportunistic build is admitted,
 *   the computed fragment chunks may be staged under
 *   `minimizers_per_fragment` when `bioseq.index.async_persistence.enabled` is on.
 *   In later resident recompute paths, if deferred choice also selects
 *   `minimizers_per_fragment`, the recomputed fragment index is written to a
 *   temp payload and staged for async persistence.
 *
 * - Active read index = `minimizers_per_sequence`.
 *   First, the RAM-first opportunistic branch may build fragment-shaped chunks
 *   and stage them under the minimizer-sequence name if memory admission succeeds and
 *   `bioseq.index.async_persistence.enabled` is on. If that is not admitted, the transient
 *   no-index minimizer-sequence branch can run when the deferred recompute gates are on
 *   and the UPDATE_ON_INSERT minimizer choice is minimizer-sequence. That branch materializes
 *   the target in memory when admitted, builds query-filtered transient segment
 *   indexes, searches, and logs `persisted=false`. This is an intentional
 *   semantic exception: the GUCs are used as eligibility gates, but no durable
 *   minimizer-sequence rows are staged by that fast path.
 *
 * - No UPDATE_ON_INSERT minimizer choice.
 *   Recompute and search normally. If `bioseq.index.recompute_async_persistence.enabled` was on
 *   but no eligible minimizer policy exists, the code logs that async-persist
 *   was skipped.
 *
 *-------------------------------------------------------------------------
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

/// Arms before fragment-anchor preparation and completes the scratch wave only
/// after the synchronous prepare/consumer scope has unwound. The scratch API is
/// idempotent/noexcept, so preparation and alignment failures both leave the
/// reusable planner clean.
using namespace ::bioseq::simd::typed_dispatch::typed_support_detail;

[[bioseq::template_flatten_when("shape", "root")]]
/// Compare every locator field before one resident target is shared by a
/// query-parallel batch. The comparison stays PostgreSQL-free so it can also
/// serve as a preflight stats before any worker is entered.
[[nodiscard]] constexpr bool same_dispatch_target_locator(
        const bioseq::storage::sequence_locator& left,
        const bioseq::storage::sequence_locator& right) noexcept {
    return typed_support_detail::same_dispatch_target_locator(left, right);
}

[[bioseq::template_flatten_when("shape", "root")]]
/// Validate the complete immutable hash-major reader identity before it is
/// borrowed concurrently. No worker may observe a partially authenticated,
/// mixed-layout or storage-profile-transcoded prepared group.
[[nodiscard]] inline bool hash_major_parallel_reader_identity_valid(
        const batch_processor::cpu_prepared_index_group& group,
        const std::uint64_t target_bp_size) noexcept {
    return typed_support_detail::hash_major_parallel_reader_identity_valid(
        group, target_bp_size);
}

[[bioseq::template_flatten_when("shape", "root")]]
/// grouped-SoA counterpart of the concurrent-reader stats. The reader owns only
/// independent fragment address plans. Query routing/count reduction is
/// transient wave state, and every persisted posting remains owned by exactly
/// one fragment.
[[nodiscard]] inline bool fragment_parallel_reader_identity_valid(
        const batch_processor::cpu_prepared_index_group& group,
        const std::uint64_t target_bp_size) noexcept {
    return typed_support_detail::fragment_parallel_reader_identity_valid(
        group, target_bp_size);
}

[[bioseq::template_flatten_when("shape", "root")]]
/// Attach the retained query-sketch stats after a worker has joined. The
/// prebuilt-minimizer entry point must not publish its own query stats; this
/// check makes duplicate attachment fail closed on the PostgreSQL thread.
inline void attach_retained_hash_major_query_stats(
        match_result_collection& collection,
        const pipe::minimizer_range_execution_stats& stats) {
    typed_support_detail::attach_retained_hash_major_query_stats(
        collection, stats);
}

/**
 * Promote one decoded sequence locator through the statement's exact source
 * provider, then expose only the canonical lazy facade to the algorithm.
 * The provider remains scan-owned; the read session owns the facade adapter
 * and is closed only after the callback has joined/drained all algorithm work.
 */
template <class p_alphabet, class p_function>
[[bioseq::template_flatten_when("shape", "root", "scanner_pg")]]
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
[[bioseq::template_flatten_when("shape", "root", "scanner_pg")]]
inline void with_pg_sequence_recipe(
        batch_processor::scanner_pg_context& context,
        const Datum recipe,
        const pg_sequence_descriptor& descriptor,
        const std::uint8_t alphabet_code,
        const std::uint8_t storage_unit_byte_width,
        p_function&& callback) {
    typed_support_detail::with_pg_sequence_recipe<p_alphabet>(
        context, recipe, descriptor, column_attno, active_read_session,
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

// Keep the PG-backed scanner row path as a separate function so GCC does not
// compile the whole typed scheduler as one very large control-flow graph.
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
    const minimap2_parameters& dispatch_params,
    const minimap2_parameters& params,
    const bool per_row_query_mode,
    std::optional<std::vector<minimap_sketch_data>>& cached_query_minimizers,
    std::optional<pipe::minimizer_range_execution_stats>&
        cached_query_minimizer_stats,
    bool& consumed_shared_cached_query_stats,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    batch_processor::scanner_pg_context* scanner_pg_ctx,
    bioseq::parallelism::global_pool& pool,
    pg_sequence_read_session* active_read_session)
{
    return typed_support_detail::process_scanner_pg_row_typed<p_sequence>(
        std::move(query_view), std::move(target_recipe_view), target_hdr, i,
        target_bp_size, min_bp, dispatch_params, params, per_row_query_mode,
        cached_query_minimizers, cached_query_minimizer_stats,
        consumed_shared_cached_query_stats, alphabet_code,
        storage_unit_byte_width, scanner_pg_ctx, pool, active_read_session);
}

template <class p_sequence>
[[bioseq::template_flatten_when("shape", "inmemory_standard")]]
[[nodiscard]] match_result_collection process_inmemory_standard_row_typed(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const minimap2_parameters& params)
{
    compact_sequence_range<p_sequence> query_seq{*row_query_hdr};
    compact_sequence_range<p_sequence> target_seq{*target_hdr};
    const auto lazy_query_view =
        bioseq::storage::make_lazy_sequence_view(query_seq);
    const auto lazy_target_view =
        bioseq::storage::make_lazy_sequence_view(target_seq);
    return minimap_port_raw_pg_checked(
        lazy_query_view, lazy_target_view, params);
}

template <class p_sequence>
[[bioseq::template_flatten_when("shape", "inmemory_segmented")]]
[[nodiscard]] match_result_collection process_inmemory_segmented_row_typed(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const minimap2_parameters& params)
{
    compact_sequence_range<p_sequence> query_seq{*row_query_hdr};
    compact_sequence_range<p_sequence> target_seq{*target_hdr};
    const auto lazy_query_view =
        bioseq::storage::make_lazy_sequence_view(query_seq);
    const auto lazy_target_view =
        bioseq::storage::make_lazy_sequence_view(target_seq);
    return minimap_port_segmented_pg_checked(
        lazy_query_view, lazy_target_view, params);
}

template <class p_sequence>
[[bioseq::template_flatten_when("shape", "inmemory_chunked")]]
[[nodiscard]] match_result_collection process_inmemory_chunked_row_typed(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const minimap2_parameters& params)
{
    compact_sequence_range<p_sequence> query_seq{*row_query_hdr};
    compact_sequence_range<p_sequence> target_seq{*target_hdr};
    match_result_collection collection;
    bioseq::storage::with_chunked_lazy_sequence(
        query_seq, target_seq,
        [&collection, &params](auto query_view, auto target_view) {
            collection = minimap_port_raw_pg_checked(
                query_view, target_view, params);
        });
    return collection;
}

template <class p_sequence>
[[bioseq::template_flatten_when("shape", "inmemory_chunked_segmented")]]
[[nodiscard]] match_result_collection process_inmemory_chunked_segmented_row_typed(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const minimap2_parameters& params)
{
    compact_sequence_range<p_sequence> query_seq{*row_query_hdr};
    compact_sequence_range<p_sequence> target_seq{*target_hdr};
    match_result_collection collection;
    bioseq::storage::with_chunked_lazy_sequence(
        query_seq, target_seq,
        [&collection, &params](auto query_view, auto target_view) {
            collection = minimap_port_segmented_pg_checked(
                query_view, target_view, params);
        });
    return collection;
}

template <class p_sequence>
[[bioseq::template_flatten_when("shape", "inmemory")]]
[[nodiscard]] match_result_collection process_inmemory_row_typed(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const sequence_size_t target_bp_size,
    const minimap2_parameters& params,
    const bool chunked_scanner_forced)
{
    const bool segmented = target_bp_size > postgres_segmentation_threshold_bp;
    if (chunked_scanner_forced) {
        return segmented
            ? process_inmemory_chunked_segmented_row(
                row_query_hdr, target_hdr, params)
            : process_inmemory_chunked_row(
                row_query_hdr, target_hdr, params);
    }
    return segmented
        ? process_inmemory_segmented_row(
            row_query_hdr, target_hdr, params)
        : process_inmemory_standard_row(
            row_query_hdr, target_hdr, params);
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
    const minimap2_parameters& base_params,
    const batch_invocation_role invocation_role,
    const std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
    batch_processor::scanner_pg_context* scanner_pg_ctx)
    -> std::map<int32_t, std::vector<match_result>>
{
    auto scanner_row = []<class... p_args>(p_args&&... args)
        -> decltype(auto) {
        return process_scanner_pg_row_typed<p_sequence>(
            std::forward<p_args>(args)...);
    };
    auto inmemory_row = []<class... p_args>(p_args&&... args)
        -> decltype(auto) {
        return process_inmemory_row_typed<p_sequence>(
            std::forward<p_args>(args)...);
    };
    return typed_support_detail::process_sequences_typed_root<p_sequence>(
        batch_size,
        alphabet_code,
        storage_unit_byte_width,
        query_hdr,
        per_row_query_hdrs,
        target_hdrs,
        query_ids,
        base_params,
        invocation_role,
        broadcast_query_ordinal,
        stats,
        scanner_pg_ctx,
        scanner_row,
        inmemory_row);
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
    const minimap2_parameters& dispatch_params,
    const minimap2_parameters& params,
    const bool per_row_query_mode,
    std::optional<std::vector<minimap_sketch_data>>& cached_query_minimizers,
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
        dispatch_params,
        params,
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
    const minimap2_parameters& params,
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
        params,
        chunked_scanner_forced);
}

[[bioseq::template_flatten_when("shape", "inmemory_standard")]]
[[bioseq::template_flatten_entry("inmemory_standard")]]
[[nodiscard]] match_result_collection
process_inmemory_standard_row(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const minimap2_parameters& params)
{
    using sequence_type = compact_sequence_stdc_instance<
        bioseq::pg_types::seq,
        alphabet,
        storage_width>;
    return process_inmemory_standard_row_typed<sequence_type>(
        row_query_hdr,
        target_hdr,
        params);
}

[[bioseq::template_flatten_when("shape", "inmemory_segmented")]]
[[bioseq::template_flatten_entry("inmemory_segmented")]]
[[nodiscard]] match_result_collection
process_inmemory_segmented_row(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const minimap2_parameters& params)
{
    using sequence_type = compact_sequence_stdc_instance<
        bioseq::pg_types::seq,
        alphabet,
        storage_width>;
    return process_inmemory_segmented_row_typed<sequence_type>(
        row_query_hdr,
        target_hdr,
        params);
}

[[bioseq::template_flatten_when("shape", "inmemory_chunked")]]
[[bioseq::template_flatten_entry("inmemory_chunked")]]
[[nodiscard]] match_result_collection
process_inmemory_chunked_row(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const minimap2_parameters& params)
{
    using sequence_type = compact_sequence_stdc_instance<
        bioseq::pg_types::seq,
        alphabet,
        storage_width>;
    return process_inmemory_chunked_row_typed<sequence_type>(
        row_query_hdr,
        target_hdr,
        params);
}

[[bioseq::template_flatten_when("shape", "inmemory_chunked_segmented")]]
[[bioseq::template_flatten_entry("inmemory_chunked_segmented")]]
[[nodiscard]] match_result_collection
process_inmemory_chunked_segmented_row(
    const bioseq::pg_types::seq* row_query_hdr,
    const bioseq::pg_types::seq* target_hdr,
    const minimap2_parameters& params)
{
    using sequence_type = compact_sequence_stdc_instance<
        bioseq::pg_types::seq,
        alphabet,
        storage_width>;
    return process_inmemory_chunked_segmented_row_typed<sequence_type>(
        row_query_hdr,
        target_hdr,
        params);
}

[[bioseq::template_flatten_when("shape", "root")]]
[[bioseq::template_flatten_entry("root")]]
[[nodiscard]] auto process_sequences(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const bioseq::pg_types::seq* query_hdr,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>& target_hdrs,
    const std::array<int32_t, dispatch_batch_capacity>& query_ids,
    const minimap2_parameters& base_params,
    const batch_invocation_role invocation_role,
    const std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
    batch_processor::scanner_pg_context* scanner_pg_ctx)
    -> std::map<int32_t, std::vector<match_result>>
{
    using sequence_type = compact_sequence_stdc_instance<
        bioseq::pg_types::seq,
        alphabet,
        storage_width>;
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
            base_params,
            typmod_operations);
        if (scanner_pg_ctx->cpu_prepared_target == nullptr) {
            throw std::logic_error{
                "Highway preparation hook did not publish a CPU target"};
        }
    }
    if (scanner_pg_ctx != nullptr
        && scanner_pg_ctx->cpu_prepared_target != nullptr
        && (!scanner_pg_ctx->cpu_prepared_target->unions_ready
            || !scanner_pg_ctx->cpu_prepared_target
                ->highway_binding_matches(base_params))) {
        throw std::logic_error{
            "prepared CPU target disagrees with the selected Highway binding"};
    }

    const auto run_pipeline = [&alphabet_code,
                               &base_params,
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
            base_params,
            invocation_role,
            broadcast_query_ordinal,
            stats,
            scanner_pg_ctx);
    };
    if (!base_params.enable_profiling) {
        return run_pipeline();
    }

    /* The one-shot summary still fires at scan end. Preparation remains
     * outside this scope, matching the authored root's previous ordering. */
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
