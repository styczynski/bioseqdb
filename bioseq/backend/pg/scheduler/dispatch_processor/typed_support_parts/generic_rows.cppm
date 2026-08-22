/**
 * @file typed_support_parts/generic_rows.cppm
 * @brief Owns CPU/SIMD parallel admission and ordered generic row dispatch.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support.generic_rows;

import std;
import bioseq.postgres.scheduler.dispatch_processor;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.policy_stats;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.pg_alignment_errors;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.source_provider_adapters;
import bioseq.highway.dispatch_contract;
import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.postgres.types.compact_sequence;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/**
 * Runs the non-Vulkan row dispatcher, retaining worker ordering, cancellation
 * points, source accounting, and the scanner/in-memory generated identities.
 */
template <class p_sequence, class p_pool, class p_scanner_row, class p_inmemory_row>
void process_generic_rows(
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
    bool& shared_target_outcome_recorded,
    const bool chunked_scanner_forced,
    const bool use_scanner_pg,
    std::optional<std::vector<minimap_sketch_data>>& cached_query_minimizers,
    std::optional<pipe::minimizer_range_execution_stats>& cached_query_minimizer_stats,
    bool& cached_query_minimizer_stats_accounted,
    p_pool& pool,
    p_scanner_row& scanner_row,
    p_inmemory_row& inmemory_row,
    std::map<int32_t, std::vector<match_result>>& results) {
    std::vector<std::uint8_t> parallel_done;
    std::vector<match_result_collection> parallel_collections;
    std::vector<minimap2_parameters> parallel_params;

    const bool may_dispatch_parallel =
        !dispatch_params.enable_profiling && !selected_vulkan_accelerator(dispatch_params) &&
        !use_scanner_pg && !chunked_scanner_forced &&
        bioseq::parallelism::has_parallel_dispatch(pool.get_mode()) &&
        pool.maximum_concurrency() > 1 && batch_size >= 2;
    if (may_dispatch_parallel) {
        parallel_done.assign(batch_size, 0);
        parallel_collections.resize(batch_size);
        parallel_params.resize(batch_size);

        pool.parallel_for_with_context(
            0,
            batch_size,
            1,
            bioseq::parallelism::range_body{
                [batch_size,
                 alphabet_code,
                 storage_unit_byte_width,
                 query_hdr,
                 per_row_query_hdrs,
                 per_row_query_mode,
                 &target_hdrs,
                 &dispatch_params,
                 &parallel_done,
                 &parallel_collections,
                 &parallel_params,
                 &inmemory_row](const std::size_t range_begin, const std::size_t range_end) {
                    for (std::size_t row_index = range_begin;
                         row_index < range_end && row_index < batch_size;
                         ++row_index) {
                        const bioseq::pg_types::seq* row_query_hdr =
                            per_row_query_mode ? (*per_row_query_hdrs)[row_index] : query_hdr;
                        const bioseq::pg_types::seq* target_hdr = target_hdrs[row_index];
                        if (row_query_hdr == nullptr || target_hdr == nullptr) {
                            continue;
                        }
                        if (row_query_hdr->alphabet_code != alphabet_code ||
                            row_query_hdr->storage_unit_byte_width != storage_unit_byte_width ||
                            target_hdr->alphabet_code != alphabet_code ||
                            target_hdr->storage_unit_byte_width != storage_unit_byte_width) {
                            continue;
                        }
                        if (target_hdr->bp_size > postgres_segmentation_threshold_bp) {
                            continue;
                        }
                        const size_t min_bp =
                            std::min<size_t>(static_cast<size_t>(row_query_hdr->bp_size),
                                             static_cast<size_t>(target_hdr->bp_size));
                        minimap2_parameters params = apply_size_heuristic(dispatch_params, min_bp);
                        parallel_collections[row_index] =
                            inmemory_row(row_query_hdr,
                                         target_hdr,
                                         static_cast<sequence_size_t>(target_hdr->bp_size),
                                         params,
                                         false);
                        parallel_params[row_index] = params;
                        parallel_done[row_index] = 1;
                    }
                }});
    }

    for (size_t row_index = 0; row_index < batch_size; ++row_index) {
        // Cancellation point at the per-row boundary. The
        // native aligner itself is NO_POSTGRES and cannot call
        // CHECK_FOR_INTERRUPTS; this lets a multi-row mmsearch honour
        // pg_cancel_backend between rows, and (with the storage-layer
        // check in fetch_payload) bounds how long a cancel waits.
        bioseq::postgres::support::invoke_pg_as_cpp([] { CHECK_FOR_INTERRUPTS(); });
        if (row_index < parallel_done.size() && parallel_done[row_index] != 0) {
            const auto requested_source = requested_target_index_source(scanner_pg_ctx);
            const bool record_this_target =
                record_target_operational_outcomes &&
                (scanner_pg_ctx == nullptr || !scanner_pg_ctx->shared_target_batch ||
                 !shared_target_outcome_recorded);
            if (record_this_target) {
                record_target_row(stats);
                record_target_index_outcome(
                    stats,
                    requested_source,
                    ::bioseq::pipeline_stats::target_index_source::recomputed);
                shared_target_outcome_recorded = true;
            }
            record_unattested_job(stats,
                                  parallel_params[row_index],
                                  requested_source,
                                  ::bioseq::pipeline_stats::target_index_source::recomputed,
                                  scanner_pg_ctx,
                                  row_index,
                                  per_row_query_mode,
                                  broadcast_query_ordinal,
                                  &parallel_collections[row_index],
                                  /*consumed_shared_cached_query_stats=*/false,
                                  cached_query_minimizer_stats_accounted);
            const sequence_size_t target_bp_size =
                static_cast<sequence_size_t>(target_hdrs[row_index]->bp_size);
            const std::size_t match_count = parallel_collections[row_index].size();
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: batch_processor row query_id={} target_bp_size={} k={} w={} "
                "min_chain_score={} min_seed_count={} prebuilt_index={} produced {} match(es) "
                "[dispatch-parallel]",
                static_cast<int>(query_ids[row_index]),
                target_bp_size,
                parallel_params[row_index].kmer_size,
                parallel_params[row_index].window_size,
                parallel_params[row_index].min_chain_score,
                parallel_params[row_index].min_seed_count,
                false,
                match_count);
            if (!parallel_collections[row_index].is_empty()) {
                results[query_ids[row_index]] = std::move(parallel_collections[row_index].results);
            }
            continue;
        }
        const bioseq::pg_types::seq* row_query_hdr =
            per_row_query_mode ? (*per_row_query_hdrs)[row_index] : query_hdr;
        const auto* query_locator = use_scanner_pg && per_row_query_mode &&
                                            scanner_pg_ctx->query_locators[row_index].has_value()
                                        ? std::addressof(*scanner_pg_ctx->query_locators[row_index])
                                        : nullptr;
        const bool has_query_locator_for_pg = query_locator != nullptr;
        const Datum query_recipe = use_scanner_pg && per_row_query_mode
                                       ? scanner_pg_ctx->query_recipe_datums[row_index]
                                       : Datum{0};
        const bool has_query_recipe = query_recipe != Datum{0};
        const Datum target_recipe =
            use_scanner_pg ? scanner_pg_ctx->target_recipe_datums[row_index] : Datum{0};
        const bool has_target_recipe = target_recipe != Datum{0};
        if (static_cast<unsigned>(row_query_hdr != nullptr) +
                static_cast<unsigned>(has_query_locator_for_pg) +
                static_cast<unsigned>(has_query_recipe) >
            1u) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_DATA_CORRUPTED, "query recipe/locator/flat dispatch ownership overlaps");
        }
        if (row_query_hdr == nullptr && !has_query_locator_for_pg && !has_query_recipe) {
            continue;
        }
        if (row_query_hdr != nullptr &&
            (row_query_hdr->alphabet_code != alphabet_code ||
             row_query_hdr->storage_unit_byte_width != storage_unit_byte_width)) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: query row query_id={} has typmod ({},{}) mismatching context "
                "({},{}); treating as WHERE FALSE",
                static_cast<int>(query_ids[row_index]),
                static_cast<unsigned>(row_query_hdr->alphabet_code),
                static_cast<unsigned>(row_query_hdr->storage_unit_byte_width),
                static_cast<unsigned>(alphabet_code),
                static_cast<unsigned>(storage_unit_byte_width));
            continue;
        }
        if (has_query_locator_for_pg && (scanner_pg_ctx->mmsearch_query_attno <= 0 ||
                                         scanner_pg_ctx->query_alphabet_code != alphabet_code ||
                                         !(scanner_pg_ctx->query_storage_unit_byte_width == 1u ||
                                           scanner_pg_ctx->query_storage_unit_byte_width == 2u ||
                                           scanner_pg_ctx->query_storage_unit_byte_width == 4u ||
                                           scanner_pg_ctx->query_storage_unit_byte_width == 8u))) {
            raise_typed_dispatch_pg_error_as_cpp(ERRCODE_DATA_CORRUPTED,
                                                 "per-row query locator metadata is invalid");
        }
        if (has_query_recipe) {
            const auto& descriptor = scanner_pg_ctx->query_recipe_descriptors[row_index];
            const bool expanded = descriptor.kind == pg_sequence_datum_kind::expanded_read_only ||
                                  descriptor.kind == pg_sequence_datum_kind::expanded_read_write;
            if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(query_recipe)) || !expanded ||
                !descriptor.alphabet_bound || descriptor.alphabet_code != alphabet_code ||
                descriptor.storage_unit_byte_width !=
                    scanner_pg_ctx->query_storage_unit_byte_width) {
                raise_typed_dispatch_pg_error_as_cpp(ERRCODE_DATA_CORRUPTED,
                                                     "per-row query recipe metadata is invalid");
            }
        }
        const bioseq::pg_types::seq* target_hdr = target_hdrs[row_index];
        // When use_scanner_pg=true and a locator was captured
        // for this row (e.g. a stub whose materialised varlena would
        // exceed MaxAllocSize, so hydration was skipped), we still have
        // enough info to dispatch via scanner_pg's PG-backed mode. Only
        // skip when NEITHER an inline target nor a locator is available.
        const bool has_locator_for_pg =
            use_scanner_pg && scanner_pg_ctx->target_locators[row_index].has_value();
        if (static_cast<unsigned>(target_hdr != nullptr) +
                static_cast<unsigned>(has_locator_for_pg) +
                static_cast<unsigned>(has_target_recipe) >
            1u) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_DATA_CORRUPTED, "target recipe/locator/flat dispatch ownership overlaps");
        }
        if (target_hdr == nullptr && !has_locator_for_pg && !has_target_recipe) {
            continue; // NULL seq column in this row → no match
        }
        if (target_hdr != nullptr &&
            (target_hdr->alphabet_code != alphabet_code ||
             target_hdr->storage_unit_byte_width != storage_unit_byte_width)) {
            /*
             * PG's typmod coercion should have rejected the
             * INSERT if the stored typmod didn't match the
             * column typmod; getting here means someone
             * wrote raw bytes via pg_catalog mischief or a
             * future cross-typmod compare path. Skip rather
             * than crash and let the tuple's mmsearch
             * qualify as FALSE.
             */
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: target row query_id={} has typmod ({},{}) mismatching context "
                "({},{}); treating as WHERE FALSE",
                static_cast<int>(query_ids[row_index]),
                static_cast<unsigned>(target_hdr->alphabet_code),
                static_cast<unsigned>(target_hdr->storage_unit_byte_width),
                static_cast<unsigned>(alphabet_code),
                static_cast<unsigned>(storage_unit_byte_width));
            continue;
        }
        if (has_target_recipe) {
            const auto& descriptor = scanner_pg_ctx->target_recipe_descriptors[row_index];
            const bool expanded = descriptor.kind == pg_sequence_datum_kind::expanded_read_only ||
                                  descriptor.kind == pg_sequence_datum_kind::expanded_read_write;
            if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(target_recipe)) || !expanded ||
                !descriptor.alphabet_bound || descriptor.alphabet_code != alphabet_code ||
                descriptor.storage_unit_byte_width != storage_unit_byte_width) {
                raise_typed_dispatch_pg_error_as_cpp(ERRCODE_DATA_CORRUPTED,
                                                     "target recipe metadata is invalid");
            }
        }
        // Construct compact_sequence_range only when target_hdr is
        // present. With a PG-backed locator we don't materialise the
        // full payload — scanner_pg streams fragment-by-fragment. Use
        // the locator's logical_length for the heuristic-relevant
        // min_bp comparison.
        const sequence_size_t target_bp_size =
            target_hdr != nullptr ? static_cast<sequence_size_t>(target_hdr->bp_size)
            : has_target_recipe
                ? static_cast<sequence_size_t>(
                      scanner_pg_ctx->target_recipe_descriptors[row_index].bp_length)
                : scanner_pg_ctx->target_locators[row_index]->logical_length;

        const sequence_size_t row_query_bp_size =
            has_query_locator_for_pg ? query_locator->logical_length
            : has_query_recipe       ? static_cast<sequence_size_t>(
                                     scanner_pg_ctx->query_recipe_descriptors[row_index].bp_length)
                               : static_cast<sequence_size_t>(row_query_hdr->bp_size);
        const size_t min_bp = std::min<size_t>(static_cast<size_t>(row_query_bp_size),
                                               static_cast<size_t>(target_bp_size));

        minimap2_parameters params = apply_size_heuristic(dispatch_params, min_bp);
        match_result_collection collection;
        bool consumed_shared_cached_query_stats = false;
        {
            if (use_scanner_pg) {
                using row_alphabet =
                    typename compact_sequence_range<p_sequence>::traits::alphabet_type;
                using row_view = bioseq::storage::lazy_sequence_view<row_alphabet>;
                /**
                 * Runs scanner_row for one query lazy view and optional target
                 * view, sharing cached query minimizers when the batch allows.
                 */
                auto execute_scanner = [target_hdr,
                                        row_index,
                                        target_bp_size,
                                        min_bp,
                                        &dispatch_params,
                                        &params,
                                        per_row_query_mode,
                                        &cached_query_minimizers,
                                        &cached_query_minimizer_stats,
                                        &consumed_shared_cached_query_stats,
                                        alphabet_code,
                                        storage_unit_byte_width,
                                        scanner_pg_ctx,
                                        &pool,
                                        &scanner_row](row_view query_lazy_view,
                                               std::optional<row_view> target_lazy_view,
                                               pg_sequence_read_session* active_session) {
                    return scanner_row(query_lazy_view,
                                       target_lazy_view,
                                       target_hdr,
                                       row_index,
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
                                       active_session);
                };
                /**
                 * Dispatches one row after binding the target from inline
                 * storage, a PG locator, or an expanded target recipe owner.
                 */
                auto execute_with_optional_target_recipe =
                    [has_target_recipe,
                     &collection,
                     &execute_scanner,
                     target_recipe,
                     scanner_pg_ctx,
                     row_index](row_view query_lazy_view,
                                pg_sequence_read_session* active_session) {
                        if (!has_target_recipe) {
                            collection =
                                execute_scanner(query_lazy_view, std::nullopt, active_session);
                            return;
                        }
                        if (active_session == nullptr) {
                            raise_typed_dispatch_pg_error_as_cpp(
                                ERRCODE_INTERNAL_ERROR, "target recipe has no shared read session");
                        }
                        const auto target_descriptor =
                            scanner_pg_ctx->target_recipe_descriptors[row_index];
                        auto* target_owner = bioseq::postgres::support::invoke_pg_as_cpp(
                            [active_session, target_recipe, target_descriptor] {
                                return ::make_pg_sequence_call_owner<row_alphabet>(
                                    *active_session, target_recipe, target_descriptor);
                            });
                        collection = execute_scanner(query_lazy_view,
                                                     std::optional<row_view>{target_owner->view()},
                                                     active_session);
                    };
                if (has_query_recipe) {
                    with_pg_sequence_recipe<row_alphabet>(
                        *scanner_pg_ctx,
                        query_recipe,
                        scanner_pg_ctx->query_recipe_descriptors[row_index],
                        scanner_pg_ctx->query_alphabet_code,
                        scanner_pg_ctx->query_storage_unit_byte_width,
                        [&execute_with_optional_target_recipe](
                            row_view query_lazy_view, pg_sequence_read_session* active_session) {
                            execute_with_optional_target_recipe(query_lazy_view, active_session);
                        });
                } else if (has_query_locator_for_pg) {
                    const std::size_t query_letters_per_unit =
                        bioseq::storage::packed_letters_per_unit(
                            scanner_pg_ctx->query_storage_unit_byte_width,
                            static_cast<std::uint8_t>(p_sequence::letter_bit_width));
                    if (query_letters_per_unit == 0u) {
                        raise_typed_dispatch_pg_error_as_cpp(
                            ERRCODE_DATA_CORRUPTED,
                            "per-row query locator has invalid packed geometry");
                    }
                    const std::uint64_t query_remainder =
                        query_locator->logical_length % query_letters_per_unit;
                    const std::uint64_t query_padding =
                        query_remainder == 0u ? 0u : query_letters_per_unit - query_remainder;
                    if (query_padding > (std::numeric_limits<std::uint16_t>::max)()) {
                        raise_typed_dispatch_pg_error_as_cpp(
                            ERRCODE_DATA_CORRUPTED, "per-row query locator padding is invalid");
                    }
                    with_source_provider_pg_locator<row_alphabet>(
                        *scanner_pg_ctx,
                        *query_locator,
                        static_cast<AttrNumber>(scanner_pg_ctx->mmsearch_query_attno),
                        scanner_pg_ctx->query_alphabet_code,
                        scanner_pg_ctx->query_storage_unit_byte_width,
                        static_cast<std::uint16_t>(query_padding),
                        scanner_pg_ctx->read_session,
                        [&execute_with_optional_target_recipe](
                            row_view query_lazy_view, pg_sequence_read_session* active_session) {
                            execute_with_optional_target_recipe(query_lazy_view, active_session);
                        });
                } else {
                    compact_sequence_range<p_sequence> query_seq{*row_query_hdr};
                    const row_view query_lazy_view =
                        bioseq::storage::make_lazy_sequence_view(query_seq);
                    if (has_target_recipe) {
                        with_pg_sequence_recipe<row_alphabet>(
                            *scanner_pg_ctx,
                            target_recipe,
                            scanner_pg_ctx->target_recipe_descriptors[row_index],
                            alphabet_code,
                            storage_unit_byte_width,
                            [&collection, &execute_scanner, query_lazy_view](
                                row_view target_lazy_view,
                                pg_sequence_read_session* active_session) {
                                collection =
                                    execute_scanner(query_lazy_view,
                                                    std::optional<row_view>{target_lazy_view},
                                                    active_session);
                            });
                    } else {
                        collection = execute_scanner(
                            query_lazy_view, std::nullopt, scanner_pg_ctx->read_session);
                    }
                }
            } else if (target_hdr != nullptr) {
                collection = inmemory_row(
                    row_query_hdr, target_hdr, target_bp_size, params, chunked_scanner_forced);
            } else if (scanner_pg_ctx != nullptr &&
                       scanner_pg_ctx->target_locators[row_index].has_value()) {
                // inmemory_forced=ON cannot satisfy its
                // force-baseline semantic on > MaxAllocSize stubs; raise.
                const int query_id = static_cast<int>(query_ids[row_index]);
                bioseq::postgres::support::invoke_pg_as_cpp([query_id, target_bp_size] {
                    ereport(ERROR,
                            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                             errmsg("forced in-memory scanner cannot materialize target stub"),
                             errdetail("row=%d target_bp=%llu exceeds MaxAllocSize",
                                       query_id,
                                       static_cast<unsigned long long>(target_bp_size)),
                             errhint("Set bioseq.testing.in_memory_scanner.enabled=off to use the "
                                     "streaming scanner_pg path.")));
                });
                pg_unreachable();
            } else {
                bioseq_log<logging_level::debug>("[bioseq] pg: row {} has no inline target and no "
                                                 "scanner_pg locator; treating as WHERE FALSE",
                                                 static_cast<int>(query_ids[row_index]));
                continue;
            }
        }

        const auto target_index_source =
            use_scanner_pg ? scanner_pg_ctx->target_index_sources[row_index]
                           : ::bioseq::pipeline_stats::target_index_source::recomputed;
        const auto requested_source = requested_target_index_source(scanner_pg_ctx);
        const bool record_this_target =
            record_target_operational_outcomes &&
            (scanner_pg_ctx == nullptr || !scanner_pg_ctx->shared_target_batch ||
             !shared_target_outcome_recorded);
        if (record_this_target) {
            record_target_row(stats);
            record_target_index_outcome(stats, requested_source, target_index_source);
            shared_target_outcome_recorded = true;
        }
        record_unattested_job(stats,
                              params,
                              requested_source,
                              target_index_source,
                              scanner_pg_ctx,
                              row_index,
                              per_row_query_mode,
                              broadcast_query_ordinal,
                              &collection,
                              consumed_shared_cached_query_stats,
                              cached_query_minimizer_stats_accounted);

        bioseq_log<logging_level::debug>(
            "[bioseq] pg: batch_processor row query_id={} target_bp_size={} k={} w={} "
            "min_chain_score={} min_seed_count={} prebuilt_index={} produced {} match(es)",
            static_cast<int>(query_ids[row_index]),
            target_bp_size,
            params.kmer_size,
            params.window_size,
            params.min_chain_score,
            params.min_seed_count,
            target_index_source == ::bioseq::pipeline_stats::target_index_source::persisted,
            collection.size());
        if (!collection.is_empty()) {
            results[query_ids[row_index]] = std::move(collection.results);
        }
    }
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
