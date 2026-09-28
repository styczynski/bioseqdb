/**
 * @file minimizer_hashmap.cppm
 * @brief Exports minimizer indexes and the modules that build and read them.
 */
export module bioseq.indexing.minimizer_hashmap;

export import bioseq.indexing.compact_hash32_direct_index;
export import bioseq.indexing.hash_major_minimizer_index;
export import bioseq.sequences.kmers.highway_minimizer_contract;
export import :exact_simd;
export import :grouped_soa;
export import :prepared_reader;
export import :compact_direct;
export import :generation;
export import :fragment_producer;

static_assert(__cplusplus >= 202302L, "C++23-or-later");

namespace bioseq::indexing::detail {
/// Private hash-table type used by the fragment index implementation.
struct minimizer_hashmap_fragment_index_table;
}
