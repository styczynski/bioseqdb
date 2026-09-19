/**
 * @file compact_direct/reader/anchor_routes.cppm
 * @brief Explicit route selection for query-wave anchor preparation.
 */
module;

#include <new>

export module bioseq.indexing.minimizer_hashmap:compact_direct.reader_anchor_routes;

import :compact_direct.reader_contract;
import :compact_direct.reader_anchor_planning;
import std;
import :exact_simd;
import :grouped_soa;
import :prepared_reader;
import :compact_direct.query_contracts;
import :compact_direct.planning_admission;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.common.checked_arithmetic;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.chaining.core;
import bioseq.common.radix_sort;
import bioseq.instrumentation;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;

export namespace bioseq::indexing {

[[nodiscard]] inline prepared_fragment_anchor_batch
prepared_minimizer_fragment_reader::prepare_fragment_anchor_batch(
    bioseq::parallelism::memory_admission_controller& admission,
    std::span<const fragment_seed_query> queries,
    std::int32_t max_occ,
    query_wave_scratch& scratch,
    bool baseline_query_plan_matcher_for_testing,
    bool verify_compositional_order_for_testing,
    minimizer_fragment_lookup_route lookup_route) const {
    if (mode_ == prepared_minimizer_fragment_reader_mode::gpu_source_only) {
        throw persisted_minimizer_lookup_error(
            "GPU-source-only fragment reader cannot execute a host anchor query");
    }
    if (lookup_route == minimizer_fragment_lookup_route::grouped_soa_control_scans &&
        backing_kind_ == backing_kind::compact_direct_only) {
        throw persisted_minimizer_lookup_error(
            "grouped-SoA control scans were explicitly selected on a "
            "compact-direct-only reader");
    }
    if (lookup_route == minimizer_fragment_lookup_route::direct_compact_hash_lookup) {
        if (baseline_query_plan_matcher_for_testing) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 direct route cannot use the grouped-SoA "
                "baseline matcher");
        }
        return prepare_fragment_anchor_batch_impl_<false, true>(
            admission, queries, max_occ, scratch, verify_compositional_order_for_testing);
    }
    if (baseline_query_plan_matcher_for_testing) {
        return prepare_fragment_anchor_batch_impl_<true, false>(
            admission, queries, max_occ, scratch, verify_compositional_order_for_testing);
    }
    return prepare_fragment_anchor_batch_impl_<false, false>(
        admission, queries, max_occ, scratch, verify_compositional_order_for_testing);
}

} // namespace bioseq::indexing
