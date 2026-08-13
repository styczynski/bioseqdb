/**
 * @file pipeline/pipe_owners_template.cppm
 * @brief Scan-only source for generated alphabet-specific pipeline owners.
 */
module;

#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.codegen.inputs.pipe_owners;

import bioseq.pipeline.pipe;
import bioseq.common.seqan_adapter;
import bioseq.storage.lazy_sequence;
import bioseq.sequences.kmers;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.gpu_minimizer_index;

namespace bioseq::simd {

static constexpr bool owner_dna5
    [[bioseq::template_flatten(
        "identity", "alphabet",
        "dna4", "false",
        "dna5", "true")]]
    = false;

static constexpr std::uint8_t owner_route
    [[bioseq::template_flatten(
        "shard", "route",
        "raw", "0",
        "segmented", "1",
        "fragment_index", "2",
        "hash_major", "3",
        "grouped_soa", "4",
        "fragment_query", "5")]]
    = 0;

namespace {

using owner_alphabet = std::conditional_t<
    owner_dna5, ::seq::dna5, ::seq::dna4>;

} // namespace

[[bioseq::template_flatten_when("route", "raw")]]
[[nodiscard, gnu::noinline]] match_result_collection minimap_port_raw_owned(
    bioseq::storage::lazy_sequence_view<owner_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<owner_alphabet> target_seq,
    const minimap2_parameters& params)
{
    return pipe_entry_detail::minimap_port_owned_pinned_shape<owner_alphabet>(
        std::move(query_seq), std::move(target_seq), params);
}

[[bioseq::template_flatten_when("route", "segmented")]]
[[nodiscard, gnu::noinline]] match_result_collection
minimap_port_segmented_owned(
    bioseq::storage::lazy_sequence_view<owner_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<owner_alphabet> target_seq,
    const minimap2_parameters& params)
{
    return pipe_entry_detail::minimap_port_segmented_owned_dispatch<
        owner_alphabet>(
            std::move(query_seq), std::move(target_seq), params);
}

[[bioseq::template_flatten_when("route", "fragment_index")]]
[[nodiscard, gnu::noinline]] match_result_collection
minimap_port_fragment_index_batch_owned(
    bioseq::storage::lazy_sequence_view<owner_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<owner_alphabet> target_seq,
    const pipe::minimizer_batch_view<
        ::bioseq::minimizers::minimap_sketch_data>& query_minimizers,
    const ::bioseq::indexing::minimizer_hashmap_fragment_index&
        prebuilt_target_index,
    const minimap2_parameters& params,
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache,
    bioseq::indexing::gpu_index_cache_identity gpu_index_identity,
    bool allow_gpu_target_index_build)
{
    return minimap2_port_with_index<
        ::bioseq::minimizers::minimap_sketch_data,
        owner_alphabet,
        ::bioseq::indexing::minimizer_hashmap_fragment_index>(
            std::move(query_seq), std::move(target_seq), query_minimizers,
            prebuilt_target_index, params, gpu_index_cache,
            gpu_index_identity, allow_gpu_target_index_build);
}

[[bioseq::template_flatten_when("route", "fragment_index")]]
[[nodiscard, gnu::noinline]] match_result_collection
minimap_port_fragment_index_vector_owned(
    bioseq::storage::lazy_sequence_view<owner_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<owner_alphabet> target_seq,
    const std::vector<::bioseq::minimizers::minimap_sketch_data>&
        query_minimizers,
    const ::bioseq::indexing::minimizer_hashmap_fragment_index&
        prebuilt_target_index,
    const minimap2_parameters& params,
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache,
    bioseq::indexing::gpu_index_cache_identity gpu_index_identity,
    bool allow_gpu_target_index_build)
{
    const pipe::minimizer_batch_view<
        ::bioseq::minimizers::minimap_sketch_data> query_batch{
        .host = std::span<const ::bioseq::minimizers::minimap_sketch_data>{
            query_minimizers.data(), query_minimizers.size()},
        .device = {},
    };
    return minimap_port_fragment_index_batch_owned(
        std::move(query_seq), std::move(target_seq), query_batch,
        prebuilt_target_index, params, gpu_index_cache,
        gpu_index_identity, allow_gpu_target_index_build);
}

[[bioseq::template_flatten_when("route", "hash_major")]]
[[nodiscard, gnu::noinline]] match_result_collection
minimap_port_hash_major_owned(
    bioseq::storage::lazy_sequence_view<owner_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<owner_alphabet> target_seq,
    const std::vector<::bioseq::minimizers::minimap_sketch_data>&
        query_minimizers,
    const ::bioseq::indexing::hash_major_minimizer_selective_reader&
        prebuilt_target_index,
    const minimap2_parameters& params,
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache,
    bioseq::indexing::gpu_index_cache_identity gpu_index_identity,
    bool allow_gpu_target_index_build)
{
    return minimap2_port_with_index<
        ::bioseq::minimizers::minimap_sketch_data,
        owner_alphabet,
        ::bioseq::indexing::hash_major_minimizer_selective_reader>(
            std::move(query_seq), std::move(target_seq), query_minimizers,
            prebuilt_target_index, params, gpu_index_cache,
            gpu_index_identity, allow_gpu_target_index_build);
}

[[bioseq::template_flatten_when("route", "grouped_soa")]]
[[nodiscard, gnu::noinline]] match_result_collection
minimap_port_grouped_soa_owned(
    bioseq::storage::lazy_sequence_view<owner_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<owner_alphabet> target_seq,
    const std::vector<::bioseq::minimizers::minimap_sketch_data>&
        query_minimizers,
    const ::bioseq::indexing::prepared_grouped_soa_query_reader&
        prebuilt_target_index,
    const minimap2_parameters& params,
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache,
    bioseq::indexing::gpu_index_cache_identity gpu_index_identity,
    bool allow_gpu_target_index_build)
{
    return minimap2_port_with_index<
        ::bioseq::minimizers::minimap_sketch_data,
        owner_alphabet,
        ::bioseq::indexing::prepared_grouped_soa_query_reader>(
            std::move(query_seq), std::move(target_seq), query_minimizers,
            prebuilt_target_index, params, gpu_index_cache,
            gpu_index_identity, allow_gpu_target_index_build);
}

[[bioseq::template_flatten_when("route", "fragment_query")]]
[[nodiscard, gnu::noinline]] match_result_collection
minimap_port_fragment_query_owned(
    bioseq::storage::lazy_sequence_view<owner_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<owner_alphabet> target_seq,
    const std::vector<::bioseq::minimizers::minimap_sketch_data>&
        query_minimizers,
    const ::bioseq::indexing::prepared_fragment_anchor_batch::query_handle&
        prebuilt_target_index,
    const minimap2_parameters& params,
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache,
    bioseq::indexing::gpu_index_cache_identity gpu_index_identity,
    bool allow_gpu_target_index_build)
{
    return minimap2_port_with_index<
        ::bioseq::minimizers::minimap_sketch_data,
        owner_alphabet,
        ::bioseq::indexing::prepared_fragment_anchor_batch::query_handle>(
            std::move(query_seq), std::move(target_seq), query_minimizers,
            prebuilt_target_index, params, gpu_index_cache,
            gpu_index_identity, allow_gpu_target_index_build);
}

} // namespace bioseq::simd
