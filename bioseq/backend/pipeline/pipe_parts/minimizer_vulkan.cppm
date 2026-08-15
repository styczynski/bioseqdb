/**
 * @file pipeline/pipe_parts/minimizer_vulkan.cppm
 * @brief Collects Vulkan minimizers through the strict device route.
 */
export module bioseq.pipeline.pipe:minimizer_vulkan;

import std;
import :stage_graph;
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
import :minimizer_request_admission;

export {

    namespace bioseq::simd::pipe {

    /// Vulkan minimap-integer minimizer extraction. Reads packed
    /// `compact_sequence_range` storage directly on the GPU. When this algorithm
    /// is selected, Vulkan execution is mandatory and failures throw.
    /// `call_result` exposes the resident device-minimizer buffer so the all-Vulkan
    /// sketch+seed path reuses it without a second upload (`may_emit_device_minimizers`).
    template <>
    struct algorithm_traits<::bioseq::simd::minimizer_algorithm::vulkan_minimap_integer> {
        struct kernel_type {
            static constexpr bool emits_scalar_minimizer_execution_stats = false;
            static constexpr bool emits_exact_simd_execution_stats = false;
            static constexpr bool emits_bounded_nthash_execution_stats = false;
            static constexpr bool emits_partial_vulkan_execution_stats = true;
            static constexpr bool may_emit_device_minimizers = true;

            template <class p_minimizer, class p_collector, class p_range>
            [[nodiscard,
              gnu::always_inline]] static bioseq::gpu::vulkan::vulkan_sketch_dispatch_token
            call(const ::bioseq::simd::minimap2_parameters& params,
                 p_collector& collector,
                 p_range& range,
                 std::size_t window_size,
                 std::size_t kmer_size) {
                if (!params.vulkan_enabled) {
                    throw_vulkan_declined("Vulkan minimizer sketch");
                }
                bioseq::gpu::vulkan::vulkan_sketch_decline decline{};
                auto token = bioseq::gpu::vulkan::collect_minimizers_vulkan<p_minimizer>(
                    collector, range, window_size, kmer_size, &decline);
                if (!token.has_value() || !token->is_internally_consistent()) {
                    throw_vulkan_sketch_declined("Vulkan minimizer sketch", decline);
                }
                return *token;
            }

            template <std::size_t p_window_size,
                      std::size_t p_kmer_size,
                      class p_minimizer,
                      class p_collector,
                      class p_range>
            [[nodiscard,
              gnu::always_inline]] static bioseq::gpu::vulkan::vulkan_sketch_dispatch_token
            call_constexpr(const ::bioseq::simd::minimap2_parameters& params,
                           p_collector& collector,
                           p_range& range) {
                return call<p_minimizer>(params, collector, range, p_window_size, p_kmer_size);
            }

            template <class p_minimizer, class p_range>
            [[nodiscard]] static std::optional<
                bioseq::gpu::vulkan::vulkan_sketch_result<p_minimizer>>
            call_result(
                const ::bioseq::simd::minimap2_parameters& params,
                p_range& range,
                std::size_t window_size,
                std::size_t kmer_size,
                bioseq::gpu::vulkan::vulkan_sketch_host_ownership_route host_ownership_route =
                    bioseq::gpu::vulkan::vulkan_sketch_host_ownership_route::collector_compatible) {
                if (!params.vulkan_enabled) {
                    throw_vulkan_declined("Vulkan minimizer sketch");
                }
                bioseq::gpu::vulkan::vulkan_sketch_decline decline{};
                auto result = bioseq::gpu::vulkan::collect_minimizers_vulkan_result<p_minimizer>(
                    range, window_size, kmer_size, &decline, host_ownership_route);
                if (!result) {
                    throw_vulkan_sketch_declined("Vulkan minimizer sketch", decline);
                }
                return result;
            }

            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return ::bioseq::simd::minimizer_algorithm_name(
                    ::bioseq::simd::minimizer_algorithm::vulkan_minimap_integer);
            }
            static void populate_runtime_params(const ::bioseq::simd::minimap2_parameters& p,
                                                std::vector<named_property>& props) noexcept {
                props.push_back({"accelerator", "vulkan"});
                props.push_back({"vulkan_enabled", p.vulkan_enabled ? "true" : "false"});
            }
            template <is_constexpr_aligner_params p_aligner_parameters>
            [[nodiscard]] static std::vector<named_property> static_properties() {
                return {
                    {"hash", "minimap-int"},
                    {"alphabet_dispatch", "runtime-shader-key"},
                    {"specialised",
                     p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
                };
            }
        };
    };

    } // namespace bioseq::simd::pipe

} // export
