/**
 * @file pipeline/pipe_parts/segmented_finalization.cppm
 * @brief Combines segment minimizers and writes completed fragment indexes.
 */
module;

#include <new>

export module bioseq.pipeline.pipe:segmented_finalization;

import std;
import :stage_graph;
import :minimizer_execution;
import :execution;
import :shape_dispatch;
import :entry_points;
import bioseq.common.logging;
import bioseq.common.templating;
import bioseq.common.seqan_adapter;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.highway.dispatch_contract;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.two_stack_minimizers;
import bioseq.sequences.cigar;
import bioseq.alignment;
import bioseq.alignment.utils;
import bioseq.alignment.ksw2_batched;
import bioseq.alignment.block_aligner;
import bioseq.chaining.core;
import bioseq.chaining.scalar;
import bioseq.chaining.simd;
import bioseq.chaining.rmq;
import bioseq.chaining.vulkan;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.stats;
import bioseq.prefilter.types;
import bioseq.prefilter.sneaky_snake;
import bioseq.prefilter.sneaky_snake.vulkan;
import bioseq.prefilter.gatekeeper;
import bioseq.prefilter.gatekeeper.vulkan;
import bioseq.common.context;
import bioseq.instrumentation;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.indexing.type;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_index;
import bioseq.gpu.vulkan.runtime;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.seeding.vulkan;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;
import :segmented_planning;

export {

    namespace bioseq::simd {

    /// Minimizers waiting to be written for one stored fragment.
    template <class p_minimizer> struct pending_fragment_build {
        sequence_size_t fragment_len_bp = 0;
        std::vector<p_minimizer> minimizers{};
    };

    /// Collect minimizers across segments and write each fragment once complete.
    /// Feed segments in core order so a flushed fragment receives no later data.
    template <class p_fragment_storage, class p_minimizer> class fragment_persist_accumulator {
      public:
        /// Borrow the destination and set the sequence and fragment lengths.
        fragment_persist_accumulator(p_fragment_storage& storage,
                                     sequence_size_t total_bp,
                                     sequence_size_t fragment_bp)
            : storage_(storage), total_bp_(total_bp), fragment_bp_(fragment_bp),
              fragment_count_(
                  static_cast<std::size_t>((total_bp + fragment_bp - 1) / fragment_bp)) {}

        /// Append a segment's minimizers to their destination fragments.
        void ingest(std::span<const segment_fragment_emission<p_minimizer>> emissions) {
            for (const segment_fragment_emission<p_minimizer>& emission : emissions) {
                pending_fragment_build<p_minimizer>& pending = pending_[emission.fragment_no];
                pending.fragment_len_bp = emission.fragment_len_bp;
                pending.minimizers.insert(pending.minimizers.end(),
                                          emission.minimizers.begin(),
                                          emission.minimizers.end());
            }
        }

        /// Write fragments ending at or before the processed core boundary.
        void flush_ready(sequence_size_t processed_core_hi_bp) {
            while (next_flush_fragment_ < fragment_count_) {
                if (fragment_hi_bp(next_flush_fragment_) > processed_core_hi_bp) {
                    break;
                }
                flush_one(next_flush_fragment_++);
            }
        }

        /// Write remaining fragments, then store the total fragment count.
        void finish() {
            while (next_flush_fragment_ < fragment_count_) {
                flush_one(next_flush_fragment_++);
            }

            const std::int64_t fragment_manifest = static_cast<std::int64_t>(fragment_count_);
            storage_.resize(bioseq::indexing::k_directory_sub_key, sizeof(std::int64_t));
            storage_.template write_to_each<std::int64_t>(
                bioseq::indexing::k_directory_sub_key,
                [fragment_manifest](
                    std::size_t chunk_number, std::int64_t* manifest_destination, std::size_t) {
                    manifest_destination[0] = fragment_manifest;
                    return chunk_number;
                });
        }

      private:
        /// Return the fragment's first base position in the full sequence.
        [[nodiscard]] sequence_size_t fragment_lo_bp(std::size_t fragment_no) const {
            return static_cast<sequence_size_t>(fragment_no) * fragment_bp_;
        }

        /// Return the base position just after the fragment, clipped to sequence end.
        [[nodiscard]] sequence_size_t fragment_hi_bp(std::size_t fragment_no) const {
            return std::min(fragment_lo_bp(fragment_no) + fragment_bp_, total_bp_);
        }

        /// Build and store one fragment index, including fragments with no minimizers.
        void flush_one(std::size_t fragment_no) {
            pending_fragment_build<p_minimizer> pending{};
            if (auto it = pending_.find(fragment_no); it != pending_.end()) {
                pending = std::move(it->second);
                pending_.erase(it);
            }
            if (pending.fragment_len_bp == 0) {
                pending.fragment_len_bp = fragment_hi_bp(fragment_no) - fragment_lo_bp(fragment_no);
            }

            minimizer_hashmap_fragment_index fragment_index;
            fragment_index.build(pending.minimizers, pending.fragment_len_bp);

            std::vector<char> serialized_index(fragment_index.serialize_binary_size());
            fragment_index.serialize_to_binary(serialized_index.data());

            const auto sub_key = static_cast<bioseq::indexing::index_sub_key_t>(fragment_no);
            storage_.resize(sub_key, serialized_index.size());
            storage_.template write_to_each<char>(
                sub_key,
                [&serialized_index](
                    std::size_t chunk_number, char* destination, std::size_t capacity_bytes) {
                    const std::size_t offset =
                        chunk_number * bioseq::indexing::k_max_index_chunk_size_bytes;
                    const std::size_t copy_bytes =
                        offset >= serialized_index.size()
                            ? 0
                            : std::min(capacity_bytes, serialized_index.size() - offset);
                    if (copy_bytes != 0) {
                        std::memcpy(destination, serialized_index.data() + offset, copy_bytes);
                    }
                    return chunk_number;
                });
        }

        p_fragment_storage& storage_;
        sequence_size_t total_bp_ = 0;
        sequence_size_t fragment_bp_ = 0;
        std::size_t fragment_count_ = 0;
        std::size_t next_flush_fragment_ = 0;
        std::map<std::size_t, pending_fragment_build<p_minimizer>> pending_{};
    };

    } // namespace bioseq::simd

} // export
