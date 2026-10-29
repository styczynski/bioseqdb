/**
 * @file minimizer_sequence_index/read_assembly/query_assembly.cppm
 * @brief Loads buckets needed by a query and builds an in-memory minimizer lookup.
 */
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>

export module bioseq.indexing.minimizer_sequence:read_assembly_query_assembly;

import bioseq.indexing.hash_major_minimizer_index;
import :read_assembly_contract;
import bioseq.indexing.minimizer_sequence.admitted_runs;
import bioseq.indexing.minimizer_sequence.bucket_codec;
import bioseq.indexing.minimizer_sequence.generation;
import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
import bioseq.common.seqan_adapter;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.type;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.pipeline_ops;
import bioseq.parallelism.pool;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;

export namespace bioseq::indexing {

/// Build a lookup for query hashes using positions from the full target range.
template <class p_storage>
inline minimizer_hashmap_fragment_index
direct_minimizer_bucket_index<p_storage>::assemble_for_query(
    p_storage& storage,
    std::span<const minimap_sketch_data> query_minimizers,
    sequence_size_t target_len,
    std::int32_t max_occ,
    minimizer_assembly_scratch& scratch) {
    return assemble_for_query_segment(
        storage, query_minimizers, target_len, max_occ, 0, target_len, scratch);
}

/// Load buckets selected by the query and keep matching positions in
/// [clip_lo_bp, clip_hi_bp). Apply max_occ to full target counts before clipping.
template <class p_storage>
inline minimizer_hashmap_fragment_index
direct_minimizer_bucket_index<p_storage>::assemble_for_query_segment(
    p_storage& storage,
    std::span<const minimap_sketch_data> query_minimizers,
    sequence_size_t target_len,
    std::int32_t max_occ,
    sequence_size_t clip_lo_bp,
    sequence_size_t clip_hi_bp,
    minimizer_assembly_scratch& scratch) {
    scratch.clear_query();
    scratch.query_kmers.reserve(query_minimizers.size());
    for (const auto& minimizer : query_minimizers) scratch.query_kmers.push_back(minimizer.kmer());
    std::sort(scratch.query_kmers.begin(), scratch.query_kmers.end());
    scratch.query_kmers.erase(std::unique(scratch.query_kmers.begin(), scratch.query_kmers.end()),
                              scratch.query_kmers.end());
    return assemble_for_query_hashes_(storage, target_len, max_occ, clip_lo_bp, clip_hi_bp, scratch);
}


} // namespace bioseq::indexing
