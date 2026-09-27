/**
 * @file pipeline/pipe_owners_template.cppm
 * @brief Input used to generate pipeline entry points for each alphabet and route.
 *
 * Code generation reads this file. The build compiles the generated
 * implementations selected by its profile.
 */
module;

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.codegen.inputs.pipe_owners;

import bioseq.pipeline.pipe;
import bioseq.common.seqan_adapter;
import bioseq.pipeline.runtime_contract;
import bioseq.storage.lazy_sequence;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.minimizer_physical_registry;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.gpu_minimizer_index;

namespace bioseq {
namespace simd {

/// Generate DNA4 and DNA5 versions of each enabled route.
static constexpr bool owner_dna5
    [[bioseq::template_flatten(
        "identity", "alphabet",
        "dna4", "false",
        "dna5", "true")]]
    = false;

/// Split each alphabet's generated implementations across six route choices.
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

/// Alphabet used by this generated implementation.
using owner_alphabet = std::conditional_t<
    owner_dna5, ::seq::dna5, ::seq::dna4>;

} // namespace

/**
 * Check the query sketch against the statement plan, then call the pipeline
 * with its selected record type. Share the host records so they stay alive
 * through index lookup and chaining.
 */
template <class p_target_index>
[[nodiscard]] static match_result_collection minimap_port_physical_owner_with_index(
    bioseq::storage::lazy_sequence_view<owner_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<owner_alphabet> target_seq,
    const ::bioseq::minimizers::minimizer_physical_batch_owner& query_minimizers,
    const p_target_index& prebuilt_target_index,
    pipe::statement_execution_plan_owner execution_plan,
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache,
    bioseq::indexing::gpu_index_cache_identity gpu_index_identity,
    const bool allow_gpu_target_index_build) {
    if (!execution_plan || !query_minimizers.valid()) {
        throw std::invalid_argument{"persisted index pipeline requires a physical query owner"};
    }
    const auto& selection = query_minimizers.layout();
    const auto& params = execution_plan->frozen_parameters;
    if (params.kmer_size <= 0 || params.window_size <= 0 ||
        selection.alphabet_id !=
            ::bioseq::minimizers::minimizer_profile_alphabet_id<owner_alphabet>() ||
        selection.requested_algorithm != params.minimizer_algorithm ||
        selection.runtime_kmer_size != static_cast<std::uint64_t>(params.kmer_size) ||
        selection.runtime_window_size != static_cast<std::uint64_t>(params.window_size)) {
        throw std::invalid_argument{
            "persisted index query owner disagrees with the statement minimizer identity"};
    }
    return ::bioseq::minimizers::visit_minimizer_physical_layout(
        selection,
        [query_seq,
         target_seq,
         &query_minimizers,
         &prebuilt_target_index,
         execution_plan,
         gpu_index_cache,
         gpu_index_identity,
         allow_gpu_target_index_build]<class p_record>() mutable {
            auto shared_records = query_minimizers.template shared_host<p_record>();
            auto batch = pipe::minimizer_batch_view<p_record>::from_shared_host(
                std::move(shared_records), 0u, query_minimizers.size());
            return minimap2_port_with_index<p_record, owner_alphabet, p_target_index>(
                std::move(query_seq),
                std::move(target_seq),
                std::move(batch),
                prebuilt_target_index,
                execution_plan,
                gpu_index_cache,
                gpu_index_identity,
                allow_gpu_target_index_build);
        });
}

/** Match raw query and target sequences using the statement's selected layout. */
[[bioseq::template_flatten_when("route", "raw")]]
[[nodiscard, gnu::noinline]] match_result_collection minimap_port_raw_owned(
    bioseq::storage::lazy_sequence_view<owner_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<owner_alphabet> target_seq,
    pipe::statement_execution_plan_owner execution_plan)
{
    return pipe_entry_detail::minimap_port_owned_pinned_shape<owner_alphabet>(
        std::move(query_seq), std::move(target_seq), execution_plan);
}

/** Match the query against target segments within the memory budget. */
[[bioseq::template_flatten_when("route", "segmented")]]
[[nodiscard, gnu::noinline]] match_result_collection
minimap_port_segmented_owned(
    bioseq::storage::lazy_sequence_view<owner_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<owner_alphabet> target_seq,
    pipe::statement_execution_plan_owner execution_plan)
{
    return pipe_entry_detail::minimap_port_segmented_owned_dispatch<
        owner_alphabet>(
            std::move(query_seq), std::move(target_seq), execution_plan);
}

/** Match against a fragment index using a query sketch with host storage. */
[[bioseq::template_flatten_when("route", "fragment_index")]]
[[nodiscard, gnu::noinline]] match_result_collection
minimap_port_fragment_index_batch_owned(
    bioseq::storage::lazy_sequence_view<owner_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<owner_alphabet> target_seq,
    pipe::minimizer_batch_view<
        ::bioseq::minimizers::minimap_sketch_data> query_minimizers,
    const ::bioseq::indexing::minimizer_hashmap_fragment_index&
        prebuilt_target_index,
    pipe::statement_execution_plan_owner execution_plan,
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache,
    bioseq::indexing::gpu_index_cache_identity gpu_index_identity,
    bool allow_gpu_target_index_build)
{
    return minimap2_port_with_index<
        ::bioseq::minimizers::minimap_sketch_data,
        owner_alphabet,
        ::bioseq::indexing::minimizer_hashmap_fragment_index>(
            std::move(query_seq), std::move(target_seq), query_minimizers,
            prebuilt_target_index, execution_plan, gpu_index_cache,
            gpu_index_identity, allow_gpu_target_index_build);
}

/** Match against a fragment index using shared host minimizers. */
[[bioseq::template_flatten_when("route", "fragment_index")]]
[[nodiscard, gnu::noinline]] match_result_collection
minimap_port_fragment_index_vector_owned(
    bioseq::storage::lazy_sequence_view<owner_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<owner_alphabet> target_seq,
    std::shared_ptr<const std::vector<::bioseq::minimizers::minimap_sketch_data>>
        query_minimizers,
    const ::bioseq::indexing::minimizer_hashmap_fragment_index&
        prebuilt_target_index,
    pipe::statement_execution_plan_owner execution_plan,
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache,
    bioseq::indexing::gpu_index_cache_identity gpu_index_identity,
    bool allow_gpu_target_index_build)
{
    return minimap2_port_with_index<
        ::bioseq::minimizers::minimap_sketch_data,
        owner_alphabet,
        ::bioseq::indexing::minimizer_hashmap_fragment_index>(
        std::move(query_seq), std::move(target_seq), query_minimizers,
        prebuilt_target_index, execution_plan, gpu_index_cache,
        gpu_index_identity, allow_gpu_target_index_build);
}

/** Match against a fragment index using the query sketch's selected record type. */
[[bioseq::template_flatten_when("route", "fragment_index")]]
[[nodiscard, gnu::noinline]] match_result_collection
minimap_port_fragment_index_physical_owned(
    bioseq::storage::lazy_sequence_view<owner_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<owner_alphabet> target_seq,
    const ::bioseq::minimizers::minimizer_physical_batch_owner& query_minimizers,
    const ::bioseq::indexing::minimizer_hashmap_fragment_index& prebuilt_target_index,
    pipe::statement_execution_plan_owner execution_plan,
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache,
    bioseq::indexing::gpu_index_cache_identity gpu_index_identity,
    bool allow_gpu_target_index_build)
{
    return minimap_port_physical_owner_with_index(
        std::move(query_seq),
        std::move(target_seq),
        query_minimizers,
        prebuilt_target_index,
        execution_plan,
        gpu_index_cache,
        gpu_index_identity,
        allow_gpu_target_index_build);
}

/** Match using the persisted index reader prepared for the query's hashes. */
[[bioseq::template_flatten_when("route", "hash_major")]]
[[nodiscard, gnu::noinline]] match_result_collection
minimap_port_hash_major_owned(
    bioseq::storage::lazy_sequence_view<owner_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<owner_alphabet> target_seq,
    const ::bioseq::minimizers::minimizer_physical_batch_owner& query_minimizers,
    const ::bioseq::indexing::hash_major_minimizer_selective_reader&
        prebuilt_target_index,
    pipe::statement_execution_plan_owner execution_plan,
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache,
    bioseq::indexing::gpu_index_cache_identity gpu_index_identity,
    bool allow_gpu_target_index_build)
{
    return minimap_port_physical_owner_with_index(
        std::move(query_seq),
        std::move(target_seq),
        query_minimizers,
        prebuilt_target_index,
        execution_plan,
        gpu_index_cache,
        gpu_index_identity,
        allow_gpu_target_index_build);
}

/** Match using prepared target postings stored as grouped coordinate arrays. */
[[bioseq::template_flatten_when("route", "grouped_soa")]]
[[nodiscard, gnu::noinline]] match_result_collection
minimap_port_grouped_soa_owned(
    bioseq::storage::lazy_sequence_view<owner_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<owner_alphabet> target_seq,
    const ::bioseq::minimizers::minimizer_physical_batch_owner& query_minimizers,
    const ::bioseq::indexing::prepared_grouped_soa_query_reader&
        prebuilt_target_index,
    pipe::statement_execution_plan_owner execution_plan,
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache,
    bioseq::indexing::gpu_index_cache_identity gpu_index_identity,
    bool allow_gpu_target_index_build)
{
    return minimap_port_physical_owner_with_index(
        std::move(query_seq),
        std::move(target_seq),
        query_minimizers,
        prebuilt_target_index,
        execution_plan,
        gpu_index_cache,
        gpu_index_identity,
        allow_gpu_target_index_build);
}

/** Match using the query's prepared fragment-anchor lookup handle. */
[[bioseq::template_flatten_when("route", "fragment_query")]]
[[nodiscard, gnu::noinline]] match_result_collection
minimap_port_fragment_query_owned(
    bioseq::storage::lazy_sequence_view<owner_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<owner_alphabet> target_seq,
    const ::bioseq::minimizers::minimizer_physical_batch_owner& query_minimizers,
    const ::bioseq::indexing::prepared_fragment_anchor_batch::query_handle&
        prebuilt_target_index,
    pipe::statement_execution_plan_owner execution_plan,
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache,
    bioseq::indexing::gpu_index_cache_identity gpu_index_identity,
    bool allow_gpu_target_index_build)
{
    return minimap_port_physical_owner_with_index(
        std::move(query_seq),
        std::move(target_seq),
        query_minimizers,
        prebuilt_target_index,
        execution_plan,
        gpu_index_cache,
        gpu_index_identity,
        allow_gpu_target_index_build);
}

} // namespace simd
} // namespace bioseq
