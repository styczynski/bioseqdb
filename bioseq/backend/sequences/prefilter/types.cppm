/**
 * @file types.cppm
 * @brief Shared CPU and Vulkan prefilter job and result layouts.
 *
 * Jobs select slices from rank-byte buffers. Results use integer fields so CPU
 * cde and Vulkan shaders can exchange the same records.
 */
module;

#include <algorithm>
#include <array>
#include <cstring>
#include <expected>
#include <optional>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

export module bioseq.prefilter.types;

import bioseq.storage.lazy_sequence_view;
import bioseq.storage.size;
import bioseq.sequences;
import bioseq.common.seqan_adapter;

export namespace bioseq::prefilter {

/**
 * @brief Records how an edit-distance prefilter handled a job.
 *
 * Unsupported inputs remain candidates for  exact alignment.
 */
enum class prefilter_status : std::uint32_t {
    success = 0u,                  ///< The selected prefilter evaluated the job.
    conservative_unequal = 1u,     ///< Only the length lower bound was evaluated.
    unsupported_conservative = 2u, ///< The job was accepted without evaluation.
};

/**
 * @brief Selects a query slice and target slice for one prefilter comparison.
 *
 * Offsets and lengths count symbols in unpacked rank-byte buffers.
 */
struct prefilter_job {
    std::uint32_t query_record_index = 0u;
    std::uint32_t target_record_index = 0u;
    std::uint32_t query_start = 0u;
    std::uint32_t target_start = 0u;
    std::uint32_t query_length = 0u;
    std::uint32_t target_length = 0u;
    std::uint32_t max_edits = 0u;
    std::uint32_t orientation = 0u;
};

/**
 * @brief Stores one prefilter decision in a format shared with Vulkan.
 *
 * A nonzero is_accepted keeps the candidate for exact alignment. A passing
 * prefilter alone does not establish a match.
 */
struct prefilter_result {
    std::uint32_t lower_bound = 0u; ///< Conservative edit-distance lower bound.
    std::uint32_t status = static_cast<std::uint32_t>(
        prefilter_status::success); ///< Encoded @ref prefilter_status.
    std::uint32_t is_accepted = 1u; ///< Zero only for a proven rejection.
    std::uint32_t diagnostics = 0u; ///< GateKeeper estimate and decision flags.
};

/**
 * @brief Borrows rank buffers and the jobs to evaluate against them.
 *
 * Each job selects its query and target independently. Result  order follows jobs.
 */
struct prefilter_batch_input {
    std::span<const storage::borrowed_rank_window_source> queries{};
    std::span<const storage::borrowed_rank_window_source> targets{};
    std::span<const prefilter_job> jobs{};
};

/** Invalid data is a batch failure, never an unsupported-algorithm keep. */
enum class prefilter_input_failure_code : std::uint8_t {
    invalid_descriptor, source_read_failed, invalid_rank,
};

/** Identifies the failing job, operand and absolute logical source position. */
struct prefilter_input_failure {
    prefilter_input_failure_code code{};
    std::size_t job_index{};
    bool target_source{};
    storage::sequence_size_t position{};
};

using prefilter_batch_result =
    std::expected<std::vector<prefilter_result>, prefilter_input_failure>;

static_assert(std::is_standard_layout_v<prefilter_job>);
static_assert(std::is_trivially_copyable_v<prefilter_job>);
static_assert(sizeof(prefilter_job) == 8u * sizeof(std::uint32_t));
static_assert(alignof(prefilter_job) == alignof(std::uint32_t));
static_assert(offsetof(prefilter_job, query_record_index) == 0u);
static_assert(offsetof(prefilter_job, target_record_index) == 4u);
static_assert(offsetof(prefilter_job, query_start) == 8u);
static_assert(offsetof(prefilter_job, target_start) == 12u);
static_assert(offsetof(prefilter_job, query_length) == 16u);
static_assert(offsetof(prefilter_job, target_length) == 20u);
static_assert(offsetof(prefilter_job, max_edits) == 24u);
static_assert(offsetof(prefilter_job, orientation) == 28u);

static_assert(std::is_standard_layout_v<prefilter_result>);
static_assert(std::is_trivially_copyable_v<prefilter_result>);
static_assert(sizeof(prefilter_result) == 4u * sizeof(std::uint32_t));
static_assert(alignof(prefilter_result) == alignof(std::uint32_t));
static_assert(offsetof(prefilter_result, lower_bound) == 0u);
static_assert(offsetof(prefilter_result, status) == 4u);
static_assert(offsetof(prefilter_result, is_accepted) == 8u);
static_assert(offsetof(prefilter_result, diagnostics) == 12u);

/** Low bits containing GateKeeper's capped mask estimate. */
inline constexpr std::uint32_t k_gatekeeper_estimate_mask = 0x0000ffffu;
/** Candidate rejection produced by GateKeeper's mask estimator. */
inline constexpr std::uint32_t k_gatekeeper_mask_rejection_candidate_mask =
    1u << 16u;
/** Exact edit-distance validation ran after a mask-based rejection candidate. */
inline constexpr std::uint32_t k_gatekeeper_exact_guard_activated_mask =
    1u << 17u;
/** An exact edit-distance check confirmed that the candidate exceeds the allowed edits. */
inline constexpr std::uint32_t k_gatekeeper_guard_confirmed_rejection_mask =
    1u << 18u;
/** A zero-threshold Hamming comparison proved the rejection. */
inline constexpr std::uint32_t k_gatekeeper_hamming_exact_rejection_mask =
    1u << 19u;
/** All GateKeeper decision flags stored in @ref prefilter_result::diagnostics. */
inline constexpr std::uint32_t k_gatekeeper_diagnostic_flags_mask =
    k_gatekeeper_mask_rejection_candidate_mask
    | k_gatekeeper_exact_guard_activated_mask
    | k_gatekeeper_guard_confirmed_rejection_mask
    | k_gatekeeper_hamming_exact_rejection_mask;
/** Every currently defined bit in @ref prefilter_result::diagnostics. */
inline constexpr std::uint32_t k_gatekeeper_diagnostic_valid_mask =
    k_gatekeeper_estimate_mask | k_gatekeeper_diagnostic_flags_mask;

/**
 * @brief Builds a prefilter result with integer status and decision fields.
 * @param lower_bound Safe edit-distance lower bound
 * @param status How the job was evaluated.
 * @param is_accepted Keep the candidate for exact alignment.
 * @param diagnostics Algorithm-specific diagnostic bits.
 * @return Result record.
 */
[[nodiscard]] constexpr prefilter_result make_prefilter_result(
    std::uint32_t lower_bound,
    prefilter_status status,
    bool is_accepted,
    std::uint32_t diagnostics = 0u) noexcept
{
    return prefilter_result{
        .lower_bound = lower_bound,
        .status = static_cast<std::uint32_t>(status),
        .is_accepted = is_accepted ? 1u : 0u,
        .diagnostics = diagnostics,
    };
}

/**
 * @brief Keeps an unsupported input as a candidate without evaluating it.
 * @return Accepted result with a zero lower bound and unsupported stats.
 */
[[nodiscard]] constexpr prefilter_result
unsupported_prefilter_result() noexcept
{
    return make_prefilter_result(
        0u, prefilter_status::unsupported_conservative, true);
}

/**
 * @brief Checks whether the selected  prefilter algorithm evaluated the job.
 * @return True only for success status.
 */
[[nodiscard]] constexpr bool is_prefilter_evaluated(
    const prefilter_result& result) noexcept
{
    return result.status == static_cast<std::uint32_t>(
        prefilter_status::success);
}

/**
 * @brief Reports whether the candidate, should proceed to exact alignment.
 * @return True when is_accepted is nonzero.
 */
[[nodiscard]] constexpr bool is_prefilter_accepted(
    const prefilter_result& result) noexcept
{
    return result.is_accepted != 0u;
}

/**
 * @brief Checks that an offset and length fit within a rank buffer.
 * @param rank_count Buffer length in symbols.
 * @param offset First symbol.
 * @param length Number of symbols.
 * @return True when the complete slice is in bounds. The check avoids overflow.
 */
[[nodiscard]] constexpr bool prefilter_slice_in_bounds(
    storage::sequence_size_t rank_count,
    std::uint32_t offset,
    std::uint32_t length) noexcept
{
    const std::size_t offset_size = static_cast<std::size_t>(offset);
    const std::size_t length_size = static_cast<std::size_t>(length);
    return offset_size <= rank_count
        && length_size <= rank_count - offset_size;
}

/**
 * @brief Checks both rank-buffer slices referenced by a job.
 * @param input Query and target buffers.
 * @param job Requested offsets and lengths.
 * @return True when both slices are in bounds.
 */
[[nodiscard]] inline bool prefilter_job_in_bounds(
    const prefilter_batch_input& input,
    const prefilter_job& job) noexcept
{
    if (job.query_record_index >= input.queries.size() ||
        job.target_record_index >= input.targets.size()) return false;
    const auto& query = input.queries[job.query_record_index];
    const auto& target = input.targets[job.target_record_index];
    if (query.read_at == nullptr || target.read_at == nullptr ||
        query.alphabet_code >= supported_biological_alphabets::alphabet_count ||
        query.alphabet_code != target.alphabet_code ||
        job.orientation != static_cast<std::uint32_t>(query.orientation) ||
        target.orientation != storage::sequence_transform::identity) return false;
    if (query.orientation != storage::sequence_transform::identity) {
        if (query.orientation != storage::sequence_transform::reverse_complement) return false;
        bool complementable = false;
        supported_biological_alphabets::dispatch_alphabet_by_code(
            query.alphabet_code, [&complementable]<class Alphabet>() {
                complementable = supported_biological_alphabets::entry_for<Alphabet>::is_complementable;
            });
        if (!complementable) return false;
    }
    return prefilter_slice_in_bounds(query.symbol_count, job.query_start, job.query_length)
        && prefilter_slice_in_bounds(target.symbol_count, job.target_start, job.target_length);
}

/**
 * One bounded native-rank window over an admitted job interval.
 * Every acquired backend span is copied before another read can evict it.
 * Allocation and read sizes are independent of the edit threshold/submaze size.
 */
struct prefilter_rank_window {
    static constexpr std::size_t chunk_size = 512u;
    storage::borrowed_rank_window_source source{};
    storage::sequence_size_t start{};
    std::size_t length{};
    std::size_t cardinality{};
    /** Minimum acquisition size; callers clamp it to the fixed scratch capacity. */
    std::size_t refill_size{chunk_size};
    std::size_t job_index{};
    bool target_source{};
    std::optional<prefilter_input_failure>* failure{};
    mutable std::array<std::uint8_t, chunk_size> scratch;
    mutable std::size_t loaded_from{};
    mutable std::size_t loaded_size{};

    /**
     * Binds an admitted job interval without initializing unread scratch bytes.
     * The caller supplies a refill size in [1, chunk_size] and retains the shared
     * failure until traversal ends. Empty cache counters guard every scratch read.
     */
    [[gnu::always_inline]] inline prefilter_rank_window(
        const storage::borrowed_rank_window_source& source_value,
        storage::sequence_size_t start_value, std::size_t length_value,
        std::size_t cardinality_value, std::size_t refill_size_value,
        std::size_t job_index_value, bool target_source_value,
        std::optional<prefilter_input_failure>& failure_value) noexcept
        : source(source_value), start(start_value), length(length_value),
          cardinality(cardinality_value), refill_size(refill_size_value),
          job_index(job_index_value), target_source(target_source_value),
          failure(&failure_value) {}

    /** Returns the job interval size, not its backend's complete length. */
    [[nodiscard, gnu::always_inline]] inline std::size_t size() const noexcept { return length; }

    /** Reports whether the complete interval is already pinned and validated. */
    [[nodiscard, gnu::always_inline]] inline bool contains(
        std::size_t position, std::size_t count) const noexcept {
        return position >= loaded_from && position - loaded_from <= loaded_size &&
            count <= loaded_size - (position - loaded_from);
    }

    /**
     * Acquires a bounded interval, retaining an existing prefix when it can grow.
     * Only newly acquired ranks are validated. Reads include at most refill_size
     * lookahead ranks, remain within this job, and are pinned before returning.
     */
    [[nodiscard, gnu::always_inline]] inline std::span<const std::uint8_t> window(
        std::size_t position, std::size_t count) const {
        if (failure->has_value()) return {};
        if (position > length || count > length - position || count > scratch.size()) {
            *failure = prefilter_input_failure{prefilter_input_failure_code::invalid_descriptor,
                job_index, target_source, start + position};
            return {};
        }
        if (count == 0u) return {};
        if (contains(position, count)) {
            return std::span<const std::uint8_t>{scratch}.subspan(position - loaded_from, count);
        }
        const bool extend_prefix = position >= loaded_from &&
            position - loaded_from <= loaded_size &&
            count <= scratch.size() - (position - loaded_from);
        if (!extend_prefix) {
            loaded_from = position;
            loaded_size = 0u;
        }
        const auto required_size = position - loaded_from + count;
        const auto read_from = loaded_from + loaded_size;
        const auto available = std::min(scratch.size() - loaded_size, length - read_from);
        const auto read_count = std::max(required_size - loaded_size,
            std::min(refill_size, available));
        const auto destination = std::span<std::uint8_t>{scratch}.subspan(loaded_size, read_count);
        const auto ranks = source.read_checked(start + read_from, read_count, destination);
        if (!ranks) {
            *failure = prefilter_input_failure{
                ranks.error() == storage::rank_window_read_failure::source_read_failed
                    ? prefilter_input_failure_code::source_read_failed
                    : prefilter_input_failure_code::invalid_descriptor,
                job_index, target_source, start + read_from};
            return {};
        }
        for (std::size_t offset = 0u; offset < ranks->size(); ++offset) {
            if ((*ranks)[offset] >= cardinality) {
                *failure = prefilter_input_failure{prefilter_input_failure_code::invalid_rank,
                    job_index, target_source, start + read_from + offset};
                return {};
            }
        }
        if (ranks->data() != destination.data()) {
            std::memcpy(destination.data(), ranks->data(), read_count);
        }
        loaded_size += read_count;
        return std::span<const std::uint8_t>{scratch}.subspan(position - loaded_from, count);
    }

    /**
     * Reuses an equivalent reader's validated cache before decoding another tile.
     * The copied ranks remain pinned while a diagonal consumer refills that reader.
     * Unrelated source descriptors always use the normal checked acquisition path.
     */
    [[nodiscard, gnu::always_inline]] inline std::span<const std::uint8_t> window(
        std::size_t position, std::size_t count, const prefilter_rank_window& cached) const {
        if (failure->has_value()) return {};
        if (contains(position, count)) {
            return std::span<const std::uint8_t>{scratch}.subspan(position - loaded_from, count);
        }
        if (!cached.failure->has_value() && source.context == cached.source.context &&
            source.read_at == cached.source.read_at &&
            source.symbol_count == cached.source.symbol_count &&
            source.alphabet_code == cached.source.alphabet_code &&
            source.orientation == cached.source.orientation && start == cached.start &&
            length == cached.length && cardinality == cached.cardinality &&
            cached.contains(position, count)) {
            const auto offset = position - cached.loaded_from;
            const auto copied_size = cached.loaded_size - offset;
            std::memcpy(scratch.data(), cached.scratch.data() + offset, copied_size);
            loaded_from = position;
            loaded_size = copied_size;
            return std::span<const std::uint8_t>{scratch}.first(count);
        }
        return window(position, count);
    }

    /** Reads one rank using bounded lookahead; inspect failure after traversal. */
    [[nodiscard, gnu::always_inline]] inline std::uint8_t operator[](std::size_t position) const {
        if (failure->has_value()) return 0u;
        if (contains(position, 1u)) return scratch[position - loaded_from];
        const auto ranks = window(position, 1u);
        return ranks.empty() ? 0u : ranks.front();
    }
};

/**
 * Validates a descriptor pair and invokes an alphabet-erased bounded algorithm.
 * refill_size controls bounded cache lookahead; it is clamped to the fixed scratch.
 */
template <class Evaluator>
[[nodiscard]] inline auto evaluate_prefilter_job(
    const prefilter_batch_input& input, std::size_t index, Evaluator&& evaluate,
    std::size_t refill_size = prefilter_rank_window::chunk_size)
    -> std::expected<std::invoke_result_t<Evaluator, const prefilter_rank_window&,
                                         const prefilter_rank_window&, const prefilter_job&>,
                     prefilter_input_failure> {
    if (index >= input.jobs.size()) {
        return std::unexpected(prefilter_input_failure{
            prefilter_input_failure_code::invalid_descriptor, index, false, 0u});
    }
    const auto& job = input.jobs[index];
    if (!prefilter_job_in_bounds(input, job)) {
        return std::unexpected(prefilter_input_failure{
            prefilter_input_failure_code::invalid_descriptor, index, false, job.query_start});
    }
    const auto& query_source = input.queries[job.query_record_index];
    std::size_t cardinality = 0u;
    supported_biological_alphabets::dispatch_alphabet_by_code(
        query_source.alphabet_code, [&cardinality]<class Alphabet>() {
            cardinality = seqan3::alphabet_size<Alphabet>;
        });
    std::optional<prefilter_input_failure> failure;
    const auto bounded_refill_size =
        std::clamp(refill_size, std::size_t{1u}, prefilter_rank_window::chunk_size);
    const prefilter_rank_window query{query_source, job.query_start, job.query_length,
        cardinality, bounded_refill_size, index, false, failure};
    const prefilter_rank_window target{input.targets[job.target_record_index],
        job.target_start, job.target_length, cardinality, bounded_refill_size, index, true, failure};
    auto result = evaluate(query, target, job);
    if (failure) return std::unexpected(*failure);
    return result;
}

/**
 * Evaluates descriptors in job order without expanding full inputs.
 * Every job gets fresh caches; refill_size controls the bounded acquisition policy.
 */
template <class Evaluator>
[[nodiscard]] inline prefilter_batch_result evaluate_prefilter_batch(
    const prefilter_batch_input& input, Evaluator&& evaluate,
    std::size_t refill_size = prefilter_rank_window::chunk_size) {
    std::vector<prefilter_result> results;
    results.reserve(input.jobs.size());
    for (std::size_t index = 0u; index < input.jobs.size(); ++index) {
        auto result = evaluate_prefilter_job(input, index, evaluate, refill_size);
        if (!result) return std::unexpected(result.error());
        results.push_back(*result);
    }
    return results;
}

/** Returns the GateKeeper estimate stored in the  low diagnostic bits. */
[[nodiscard]] constexpr std::uint32_t gatekeeper_diagnostic_estimate(
    const prefilter_result& result) noexcept
{
    return result.diagnostics & k_gatekeeper_estimate_mask;
}

/** Reports whether the GateKeeper mask score proposed rejection. */
[[nodiscard]] constexpr bool is_gatekeeper_mask_rejection_candidate(
    const prefilter_result& result) noexcept
{
    return (result.diagnostics
            & k_gatekeeper_mask_rejection_candidate_mask) != 0u;
}

/** Reports whether an exact edit-distance check followed the mask estimate. */
[[nodiscard]] constexpr bool is_gatekeeper_exact_guard_activated(
    const prefilter_result& result) noexcept
{
    return (result.diagnostics
            & k_gatekeeper_exact_guard_activated_mask) != 0u;
}

/** Reports whether the exact edit-distance check confirmed rejection. */
[[nodiscard]] constexpr bool is_gatekeeper_guard_confirmed_rejection(
    const prefilter_result& result) noexcept
{
    return (result.diagnostics
            & k_gatekeeper_guard_confirmed_rejection_mask) != 0u;
}

/** Reports whether a mismatch rejected a job allowing zero edits. */
[[nodiscard]] constexpr bool is_gatekeeper_hamming_exact_rejection(
    const prefilter_result& result) noexcept
{
    return (result.diagnostics
            & k_gatekeeper_hamming_exact_rejection_mask) != 0u;
}

} // namespace bioseq::prefilter
