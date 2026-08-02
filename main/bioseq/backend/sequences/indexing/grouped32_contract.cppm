/**
 * @file grouped32_contract.cppm
 * @brief Target-neutral ABI shared by grouped32 readers and Highway kernels.
 *
 * This module intentionally contains only value types and one function-pointer
 * signature.  Persisted-index readers and generated fixed-target partitions
 * may therefore share the grouped32 validation ABI without importing the large
 * minimizer-hashmap implementation BMI or exposing Highway descriptor types.
 */
module;

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

export module bioseq.indexing.grouped32_contract;

export namespace bioseq::indexing {

/** Fixed byte width of one persisted grouped32 local position. */
inline constexpr std::size_t kMinimizerGrouped32PositionEncodedBytes = 4u;

/** Largest base-pair offset representable before the strand bit is added. */
inline constexpr std::uint32_t kMinimizerGrouped32MaxLocalBp = 0x7fffffffu;

/** Half-open base-pair core owned by one persisted position segment. */
struct MinimizerPositionSegmentCore {
    std::uint64_t begin_bp = 0;
    std::uint64_t end_bp = 0;

    [[nodiscard]] bool operator==(
        const MinimizerPositionSegmentCore&) const = default;
};

/** Logical representation of one fixed-width grouped-u32 control. */
struct MinimizerGrouped32Control {
    std::uint32_t hash = 0;
    std::uint32_t position_offset = 0;
    std::uint32_t position_count = 0;

    [[nodiscard]] bool operator==(
        const MinimizerGrouped32Control&) const = default;
};

/** Per-reader evidence emitted by grouped32 validation operations. */
struct PreparedGrouped32QueryReaderSimdRouteEvidence {
    std::uint64_t attempted_spans = 0;
    std::uint64_t accepted_spans = 0;
    std::uint64_t rejected_spans = 0;
    std::uint64_t scalar_required_spans = 0;
    std::uint64_t vectorized_words = 0;
    std::uint64_t selected_hwy_target = 0;
    std::uint8_t selected_vector_stream_width = 0;
    bool saturated = false;
};

/** Target-neutral outcome of one non-scatter grouped32 validation attempt. */
enum class MinimizerGrouped32NonScatterValidation : std::uint8_t {
    scalar_required,
    accepted,
    rejected,
};

/** Target-neutral evidence returned by one grouped32 validation attempt. */
struct MinimizerGrouped32NonScatterValidationResult {
    MinimizerGrouped32NonScatterValidation validation =
        MinimizerGrouped32NonScatterValidation::scalar_required;
    std::uint64_t vectorized_words = 0;
    std::uint64_t selected_hwy_target = 0;
    std::uint8_t selected_vector_stream_width = 0;
};

/** Exact neutral callable ABI implemented by each generated Highway target. */
using MinimizerGrouped32ValidateNonScatterLocal32W2Kernel =
    MinimizerGrouped32NonScatterValidationResult (*)(
        std::span<const std::byte>,
        const MinimizerGrouped32Control&,
        const MinimizerPositionSegmentCore&) noexcept;

static_assert(std::is_standard_layout_v<MinimizerPositionSegmentCore>);
static_assert(std::is_trivially_copyable_v<MinimizerPositionSegmentCore>);
static_assert(std::is_standard_layout_v<MinimizerGrouped32Control>);
static_assert(std::is_trivially_copyable_v<MinimizerGrouped32Control>);
static_assert(std::is_standard_layout_v<
    PreparedGrouped32QueryReaderSimdRouteEvidence>);
static_assert(std::is_trivially_copyable_v<
    PreparedGrouped32QueryReaderSimdRouteEvidence>);
static_assert(std::is_standard_layout_v<
    MinimizerGrouped32NonScatterValidationResult>);
static_assert(std::is_trivially_copyable_v<
    MinimizerGrouped32NonScatterValidationResult>);

} // namespace bioseq::indexing
