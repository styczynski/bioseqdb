/**
 * @file highway_dispatch_contract.cppm
 * @brief Target-neutral ABI for the one-shot Highway dispatch root.
 *
 * This interface deliberately does not include Highway. Generated target
 * primaries import it without exposing target-local vector or descriptor types
 * through their BMIs.
 */
module;

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

export module bioseq.highway.dispatch_contract;

export import :pipeline;
export import bioseq.sequences.kmers.highway_contract;

import bioseq.sequences.prefilters.sneaky_snake;
import bioseq.sequences.chaining.simd;
import bioseq.indexing.minimizer_hashmap;

export namespace bioseq::simd_dispatch {

/** Coarse scheduler kernels whose implementation is bound to one HWY target. */
enum class HighwayKernelId : std::uint8_t {
    minimizer_reference_minimap_int,
    minimizer_two_stack_nthash,
    minimizer_paper_nthash,
    indexing_grouped32_validate,
    chaining_chain_anchors_simd,
    prefilter_sneaky_snake_simd_batch,
    alignment_ksw2,
    alignment_ksw2_batched,
    alignment_block_aligner,
    count,
};

inline constexpr std::size_t highway_kernel_count =
    static_cast<std::size_t>(HighwayKernelId::count);

using HighwayKernelSet = std::bitset<highway_kernel_count>;

/** Exact neutral ABI for one fixed-target SneakySnake batch implementation. */
using SneakySnakeSimdBatchKernel =
    std::vector<::bioseq::sequences::prefilters::PrefilterResult> (*)(
        const ::bioseq::sequences::prefilters::PrefilterBatchInput&,
        ::bioseq::sequences::prefilters::SneakySnakeOptions);

/** Coarse target-bound functions shared by multiple scheduler families. */
struct CommonKernels final {
    SneakySnakeSimdBatchKernel sneaky_snake_simd_batch = nullptr;
};

static_assert(std::is_standard_layout_v<CommonKernels>);
static_assert(std::is_trivially_copyable_v<CommonKernels>);

/** Fixed-target DP chaining functions. */
struct ChainingKernels final {
    ::bioseq::chaining::ChainAnchorsSimdKernel chain_anchors_simd = nullptr;
};

/** Fixed-target persisted-index validation functions. */
struct IndexingKernels final {
    ::bioseq::indexing::
        MinimizerGrouped32ValidateNonScatterLocal32W2Kernel
            validate_non_scatter_local32_w2 = nullptr;
};

static_assert(std::is_standard_layout_v<ChainingKernels>);
static_assert(std::is_trivially_copyable_v<ChainingKernels>);
static_assert(std::is_standard_layout_v<IndexingKernels>);
static_assert(std::is_trivially_copyable_v<IndexingKernels>);

/** Stable EXPLAIN spelling for one coarse target-bound function. */
[[nodiscard]] constexpr std::string_view highway_kernel_name(
    const HighwayKernelId kernel) noexcept
{
    switch (kernel) {
        case HighwayKernelId::minimizer_reference_minimap_int:
            return "minimizer::reference_minimap_int";
        case HighwayKernelId::minimizer_two_stack_nthash:
            return "minimizer::two_stack_nthash";
        case HighwayKernelId::minimizer_paper_nthash:
            return "minimizer::paper_nthash";
        case HighwayKernelId::indexing_grouped32_validate:
            return "indexing::grouped32_validate";
        case HighwayKernelId::chaining_chain_anchors_simd:
            return "chaining::chain_anchors_simd";
        case HighwayKernelId::prefilter_sneaky_snake_simd_batch:
            return "prefilter::sneaky_snake_simd_batch";
        case HighwayKernelId::alignment_ksw2:
            return "alignment::ksw2";
        case HighwayKernelId::alignment_ksw2_batched:
            return "alignment::ksw2_batched";
        case HighwayKernelId::alignment_block_aligner:
            return "alignment::block_aligner";
        case HighwayKernelId::count:
            break;
    }
    return "unknown";
}

/** Construct a one-element kernel set without exposing bit indices at callers. */
[[nodiscard]] constexpr HighwayKernelSet highway_kernel_set(
    const HighwayKernelId kernel) noexcept
{
    HighwayKernelSet result;
    const auto index = static_cast<std::size_t>(kernel);
    if (index < result.size()) {
        result.set(index);
    }
    return result;
}

/**
 * Immutable root table selected by the single runtime dispatch.
 *
 * Callable families are added only after their signatures have been moved to
 * neutral modules. Until then, this exact availability set is sufficient for
 * generated-binder closure checks and execution-plan reporting; it avoids
 * freezing erased or target-local placeholder ABIs into the public contract.
 */
struct HighwayKernelTable final {
    HighwayKernelSet available_kernels{};
    ::bioseq::minimizers::MinimizerKernels minimizers{};
    CommonKernels common{};
    ChainingKernels chaining{};
    IndexingKernels indexing{};
    std::span<const ::bioseq::simd::AlignmentTableEntry>
        alignment_by_alphabet{};

    [[nodiscard]] bool contains(HighwayKernelId kernel) const noexcept {
        const auto index = static_cast<std::size_t>(kernel);
        return index < available_kernels.size() && available_kernels[index];
    }

    [[nodiscard]] bool contains_all(
        const HighwayKernelSet& required) const noexcept
    {
        return (available_kernels & required) == required;
    }
};

/** Neutral value returned by each generated target's binder. */
struct HighwayTargetBinding final {
    std::int64_t target = 0;
    const char* target_name = nullptr;
    const HighwayKernelTable* kernels = nullptr;
};

static_assert(std::is_standard_layout_v<HighwayTargetBinding>);
static_assert(std::is_trivially_copyable_v<HighwayTargetBinding>);

/**
 * Build dispatch inventory from the registry's exact compiled-target mask.
 * The unmodified CPU/OS mask comes from hwy::SupportedTargets().
 */
[[nodiscard]] HighwayDispatchInfo highway_dispatch_info(
    std::int64_t compiled_targets) noexcept;

/** Format each single target bit in Highway priority order via TargetName(). */
[[nodiscard]] std::vector<std::string_view> highway_target_names(
    std::int64_t mask);

}  // namespace bioseq::simd_dispatch
