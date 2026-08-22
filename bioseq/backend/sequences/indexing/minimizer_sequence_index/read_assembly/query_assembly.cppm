/**
 * @file minimizer_sequence_index/read_assembly/query_assembly.cppm
 * @brief Selective bucket reads and query-local minimizer index assembly.
 */
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <iterator>
#include <libassert/assert.hpp>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.indexing.minimizer_sequence:read_assembly_query_assembly;

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

/// Load only the persisted buckets referenced by the query minimizers.
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

/// Load query-touched buckets and retain positions in one target range.
/// Occurrence filtering still uses the complete persisted bucket counts.
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
    using detail::k_encoded_header_bytes;
    using detail::k_encoded_bucket_bytes;
    scratch.clear_query();
    clip_hi_bp = std::min(clip_hi_bp, target_len);
    if (clip_lo_bp > clip_hi_bp) {
        clip_lo_bp = clip_hi_bp;
    }
    // 1. read the directory into an owned buffer (the directory view copies it).
    const char* dblob = storage.template read_chunk<char>(k_directory_sub_key, 0);
    const std::size_t dbytes = storage.chunk_byte_size(k_directory_sub_key, 0);
    storage.validate_minimizer_directory_or_error(dblob, dbytes);
    direct_minimizer_bucket_directory_view dir{dblob, dbytes};
    const std::uint32_t max_occ_threshold =
        detail::effective_max_occ_threshold(max_occ, dir.is_gpu_built());
    // 2. collect distinct query kmers + the distinct bucket sub_keys they touch.
    auto& qkmers = scratch.query_kmers;
    auto& wanted = scratch.wanted_sub_keys;
    for (const auto& qm : query_minimizers) {
        qkmers.push_back(qm.kmer());
        if (const direct_minimizer_bucket_directory_entry* e = dir.find(qm.kmer()))
            wanted.push_back(e->sub_key);
    }
    auto sort_unique = [](auto& v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    };
    sort_unique(qkmers);
    sort_unique(wanted);
    // 3. read only those buckets; deserialise; drop kmers with global count > max_occ.
    auto& pairs = scratch.pairs;
    auto emit_kept = [&qkmers, &max_occ_threshold, &pairs, &clip_lo_bp, &clip_hi_bp](
                         std::uint64_t kmer, std::uint32_t pos_count, const std::uint64_t* gpos) {
        if (!std::binary_search(qkmers.begin(), qkmers.end(), kmer))
            return; // clip to QUERY kmers
        if (pos_count > max_occ_threshold)
            return; // drop over-frequent kmer
        for (std::uint32_t j = 0; j < pos_count; ++j) {
            const sequence_size_t pos_bp = static_cast<sequence_size_t>(gpos[j] >> 1);
            if (pos_bp < clip_lo_bp || pos_bp >= clip_hi_bp) {
                continue;
            }
            pairs.emplace_back(kmer, gpos[j]);
        }
    };
    for (std::int64_t sk : wanted) {
        const std::size_t nc = storage.number_of_chunks(sk);
        if (nc == 0)
            storage.index_error(
                "minimizers_per_sequence: directory references a bucket with no chunks");
        if (nc > 1) {
            // A heavy-hitter bucket contains one k-mer; metadata gives its exact count.
            std::size_t exact = 0;
            for (std::size_t c = 0; c < nc; ++c) {
                storage.validate_minimizer_chunk_metadata_or_error(
                    sk, c, k_encoded_header_bytes + k_encoded_bucket_bytes, alignof(std::uint64_t));
                const std::size_t cb = storage.chunk_byte_size(sk, c);
                exact +=
                    (cb - k_encoded_header_bytes - k_encoded_bucket_bytes) / sizeof(std::uint64_t);
            }
            if (exact > static_cast<std::size_t>(max_occ_threshold))
                continue;                          // drop over-frequent kmer
            for (std::size_t c = 0; c < nc; ++c) { // materialise EVERY chunk
                const std::size_t cb = storage.chunk_byte_size(sk, c);
                const char* p = storage.template read_chunk<char>(sk, c);
                const auto h = storage.validate_minimizer_blob_or_error(p, cb, 1);
                if (h.bucket_count != 1)
                    storage.index_error(
                        "assemble_for_query: multi-chunk bucket with bucket_count!=1 (byte-math invalid)");
                wire_for_each_bucket(p, cb, scratch.bucket_positions, emit_kept);
            }
            continue;
        }
        const std::size_t cb0 = storage.chunk_byte_size(sk, 0);
        const char* p0 = storage.template read_chunk<char>(sk, 0);
        storage.validate_minimizer_blob_or_error(p0, cb0);
        wire_for_each_bucket(p0, cb0, scratch.bucket_positions, emit_kept);
    }
    // 4. build the single pipeline IndexT.
    minimizer_hashmap_fragment_index idx;
    idx.build_from_pairs(pairs, target_len);
    return idx;
}

} // namespace bioseq::indexing
