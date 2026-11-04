/**
 * @file utils.cppm
 * @brief Substitution scores shared by scalar and SIMD alignment code.
 */
module;

#include <algorithm>
#include <array>
#include <expected>
#include <span>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

export module bioseq.alignment.utils;

import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.simd.utils;

export namespace bioseq::simd {

/** A typed rejection shared by prepared scoring and CPU/GPU alignment adapters. */
enum class alignment_contract_error : std::uint8_t {
    none, gap_penalty, block_geometry, x_drop_threshold, stage_mode,
    lane_shape, coordinates, scoring_matrix, trace_accounting,
    score_range, score_accumulation, invalid_matrix_layout, orientation,
    source_read_failed, device_capability, memory_admission,
};

/** Whether every biological diagonal/off-diagonal and synthetic score is uniform. */
enum class substitution_kind : std::uint8_t { uniform = 0, dense = 1 };

/** Matrix extrema and the uniform-scoring proof computed once during preparation. */
struct substitution_summary final {
    std::int32_t minimum = 0;
    std::int32_t maximum = 0;
    std::int32_t equal_score = 0;
    std::int32_t unequal_score = 0;
    std::int32_t unknown_score = 0;
    bool uniform_diagonal = false;
    bool uniform_off_diagonal = false;
    bool uniform_unknown = false;
};

/**
 * Return whether the candidate gap pair costs no more at every integer length >= 1.
 * A length-n gap costs (open + extend) + (n - 1) * extend, so compare its
 * first-symbol cost and slope. Widen both sums before comparing caller-supplied costs.
 */
[[nodiscard]] constexpr bool affine_gap_dominates(
    std::int32_t candidate_open, std::int32_t candidate_extend,
    std::int32_t other_open, std::int32_t other_extend) noexcept {
    return std::int64_t{candidate_open} + candidate_extend
               <= std::int64_t{other_open} + other_extend
        && candidate_extend <= other_extend;
}

/**
 * Check accumulation and optional int8 difference-encoding bounds before allocation.
 * Gap pairs are positive extension/nonnegative opening costs; omitted long costs use
 * the single pair. All arithmetic is widened before multiplication or normalization.
 */
[[nodiscard]] inline std::expected<void, alignment_contract_error>
validate_alignment_numeric_contract(
    const substitution_summary& scores,
    std::size_t query_length, std::size_t target_length,
    std::int32_t gap_open, std::int32_t gap_extend,
    std::int32_t end_bonus = 0, std::int32_t z_drop = -1,
    std::int32_t gap_open_long = -1, std::int32_t gap_extend_long = -1,
    bool require_narrow_differences = true) noexcept {
    if (gap_open < 0 || gap_extend <= 0
        || (gap_open_long < 0 && (gap_open_long != -1 || gap_extend_long != -1))
        || (gap_open_long >= 0 && gap_extend_long <= 0)) {
        return std::unexpected(alignment_contract_error::gap_penalty);
    }
    if (gap_open_long >= 0) {
        if (affine_gap_dominates(gap_open, gap_extend, gap_open_long, gap_extend_long)) {
            gap_open_long = gap_open;
            gap_extend_long = gap_extend;
        } else if (affine_gap_dominates(gap_open_long, gap_extend_long, gap_open, gap_extend)) {
            gap_open = gap_open_long;
            gap_extend = gap_extend_long;
        }
    }
    const std::int64_t long_open = gap_open_long < 0 ? gap_open : gap_open_long;
    const std::int64_t long_extend = gap_open_long < 0 ? gap_extend : gap_extend_long;
    const std::int64_t short_cost = std::int64_t{gap_open} + gap_extend;
    const std::int64_t long_cost = long_open + long_extend;
    const std::int64_t maximum_cost = std::max(short_cost, long_cost);
    const std::int64_t minimum_cost = std::min(short_cost, long_cost);
    const std::int64_t magnitude = std::max(
        -std::int64_t{scores.minimum}, std::int64_t{scores.maximum});
    if (scores.minimum < -127 || scores.maximum > 127
        || scores.minimum > scores.maximum || scores.maximum < 0
        || (require_narrow_differences
            && (scores.minimum < -2 * minimum_cost
                || 4 * maximum_cost + 2 * magnitude > 120))) {
        return std::unexpected(alignment_contract_error::score_range);
    }
    constexpr std::uint64_t score_limit = std::uint64_t{1} << 29;
    const std::uint64_t per_step = static_cast<std::uint64_t>(magnitude
        + std::max(std::int64_t{gap_open}, long_open)
        + std::max(std::int64_t{gap_extend}, long_extend));
    const std::int64_t signed_bonus = end_bonus;
    const std::uint64_t extras = static_cast<std::uint64_t>(
        signed_bonus < 0 ? -signed_bonus : signed_bonus)
        + static_cast<std::uint64_t>(std::max(z_drop, 0));
    if (extras >= score_limit
        || query_length >= score_limit || target_length >= score_limit
        || query_length > score_limit - target_length
        || (per_step != 0 && query_length + target_length
            > (score_limit - 1 - extras) / per_step)) {
        return std::unexpected(alignment_contract_error::score_accumulation);
    }
    return {};
}

/** Immutable substitution scores with synchronized forward and transposed byte tables. */
template <sequence_alphabet p_alphabet, std::size_t p_alphabet_dimension>
struct scoring_matrix_storage {
    using alphabet_type = p_alphabet;
    static constexpr std::size_t k_alphabet_dimension = p_alphabet_dimension;
    static_assert(p_alphabet_dimension > 1);
    static constexpr std::size_t k_entry_count = p_alphabet_dimension * p_alphabet_dimension;
    static constexpr int k_dimension = static_cast<int>(p_alphabet_dimension);
    static constexpr std::size_t k_simd_rows = p_alphabet_dimension + 1;
    // Highway byte tables select one of sixteen entries per 128-bit lookup block.
    static constexpr std::size_t k_simd_lookup_count = (k_simd_rows + 15) / 16;
    static constexpr std::size_t k_simd_columns = k_simd_lookup_count * 16;
    static constexpr std::uint8_t k_null_byte = static_cast<std::uint8_t>(p_alphabet_dimension);
    static_assert(p_alphabet_dimension < std::numeric_limits<std::uint8_t>::max());

private:
    std::array<std::int8_t, k_entry_count> entries_{};
    alignas(64) std::array<std::int8_t, k_simd_rows * k_simd_columns> simd_scores_{};
    alignas(64) std::array<std::int8_t, k_simd_rows * k_simd_columns> transposed_scores_{};
    substitution_summary summary_{};
    substitution_kind kind_ = substitution_kind::dense;
    bool prepared_ = false;

public:
    /** Read the target-row/query-column substitution score. */
    [[nodiscard]] constexpr std::int8_t at(std::size_t row, std::size_t column) const noexcept {
        return entries_[row * p_alphabet_dimension + column];
    }
    /** Read the complete logical table, including the synthetic unknown rank. */
    [[nodiscard]] constexpr std::span<const std::int8_t> score_entries() const noexcept {
        return entries_;
    }
    /** Return the immutable proof and extrema computed by the checked factory. */
    [[nodiscard]] constexpr const substitution_summary& summary() const noexcept { return summary_; }
    /** Select the uniform compare path only when the entire matrix supports it. */
    [[nodiscard]] constexpr substitution_kind kind() const noexcept { return kind_; }
    /** Return the uniform diagonal representative; dense callers use maximum instead. */
    [[nodiscard]] constexpr std::int8_t match_score() const noexcept { return entries_[0]; }
    /** Return the uniform off-diagonal representative. */
    [[nodiscard]] constexpr std::int8_t mismatch_score() const noexcept { return entries_[1]; }
    /** Return the synthetic unknown representative, including a deliberate zero. */
    [[nodiscard]] constexpr std::int8_t wildcard_score() const noexcept { return entries_.back(); }
    /** Access one padded row for Highway byte-table lookup. */
    [[nodiscard]] const std::int8_t* simd_row_pointer(std::size_t row, bool transposed = false) const noexcept {
        return (transposed ? transposed_scores_.data() : simd_scores_.data()) + row * k_simd_columns;
    }
    /** Factories are the only writers; successful preparation proves both layouts. */
    [[nodiscard, gnu::always_inline]] bool is_block_layout_valid() const noexcept { return prepared_; }
    /** Hash alphabet rank characters and scores for prepared execution-plan identity. */
    [[nodiscard]] std::uint64_t block_semantic_identity() const noexcept {
        std::uint64_t hash = 1469598103934665603ULL;
        const auto mix = [&hash](std::uint64_t value) noexcept { hash ^= value; hash *= 1099511628211ULL; };
        mix(p_alphabet_dimension);
        if constexpr (alphabet_in_supported_set<supported_biological_alphabets, p_alphabet>)
            mix(supported_biological_alphabets::template index_of<p_alphabet>());
        for (std::size_t rank = 0; rank < seqan3::alphabet_size<p_alphabet>; ++rank) {
            mix(static_cast<std::uint8_t>(seqan3::to_char(seqan3::assign_rank_to(rank, p_alphabet{}))));
        }
        for (const std::int8_t score : entries_) mix(static_cast<std::uint8_t>(score));
        return hash;
    }
    /** Validate dense int32 input once, then construct every read-only kernel layout. */
    [[nodiscard]] static std::expected<scoring_matrix_storage, alignment_contract_error>
    from_entries(const std::array<std::int32_t, k_entry_count>& values) {
        scoring_matrix_storage matrix{};
        matrix.summary_.minimum = 127;
        matrix.summary_.maximum = -127;
        matrix.summary_.equal_score = values.front();
        matrix.summary_.unequal_score = values[1];
        matrix.summary_.unknown_score = values.back();
        matrix.summary_.uniform_diagonal = true;
        matrix.summary_.uniform_off_diagonal = true;
        matrix.summary_.uniform_unknown = true;
        matrix.simd_scores_.fill(std::numeric_limits<std::int8_t>::min());
        matrix.transposed_scores_.fill(std::numeric_limits<std::int8_t>::min());
        constexpr std::size_t biological_ranks = p_alphabet_dimension - 1;
        for (std::size_t row = 0; row < p_alphabet_dimension; ++row) {
            for (std::size_t column = 0; column < p_alphabet_dimension; ++column) {
                const auto value = values[row * p_alphabet_dimension + column];
                if (value < -127 || value > 127) return std::unexpected(alignment_contract_error::score_range);
                matrix.entries_[row * p_alphabet_dimension + column] = static_cast<std::int8_t>(value);
                matrix.simd_scores_[row * k_simd_columns + column] = static_cast<std::int8_t>(value);
                matrix.transposed_scores_[column * k_simd_columns + row] = static_cast<std::int8_t>(value);
                matrix.summary_.minimum = std::min(matrix.summary_.minimum, value);
                matrix.summary_.maximum = std::max(matrix.summary_.maximum, value);
                if (row == biological_ranks || column == biological_ranks)
                    matrix.summary_.uniform_unknown &= value == matrix.summary_.unknown_score;
                else if (row == column) matrix.summary_.uniform_diagonal &= value == matrix.summary_.equal_score;
                else matrix.summary_.uniform_off_diagonal &= value == matrix.summary_.unequal_score;
            }
        }
        matrix.kind_ = matrix.summary_.uniform_diagonal && matrix.summary_.uniform_off_diagonal
            && matrix.summary_.uniform_unknown ? substitution_kind::uniform : substitution_kind::dense;
        matrix.prepared_ = true;
        return matrix;
    }
    /** Validate magnitudes before sign normalization and prepare uniform scoring. */
    [[nodiscard]] static std::expected<scoring_matrix_storage, alignment_contract_error>
    generate(std::int32_t match, std::int32_t mismatch_penalty, std::int32_t ambiguity_penalty) {
        if (match < -127 || match > 127 || mismatch_penalty < -127 || mismatch_penalty > 127
            || ambiguity_penalty < -127 || ambiguity_penalty > 127)
            return std::unexpected(alignment_contract_error::score_range);
        const auto reward = match < 0 ? -match : match;
        const auto mismatch = mismatch_penalty > 0 ? -mismatch_penalty : mismatch_penalty;
        const auto unknown = ambiguity_penalty > 0 ? -ambiguity_penalty : ambiguity_penalty;
        std::array<std::int32_t, k_entry_count> values{};
        constexpr std::size_t biological_ranks = p_alphabet_dimension - 1;
        for (std::size_t row = 0; row < p_alphabet_dimension; ++row)
            for (std::size_t column = 0; column < p_alphabet_dimension; ++column)
                values[row * p_alphabet_dimension + column] = row == biological_ranks || column == biological_ranks
                    ? unknown : row == column ? reward : mismatch;
        return from_entries(values);
    }
};

/**
 * @brief Selects score storage from an alphabet or compact-sequence source.
 *
 * @tparam p_source Alphabet or compatible compact-sequence source.
 * @tparam p_alphabet_dimension Number of matrix ranks.
 */
template <
    supports_compact_sequence_range p_source,
    std::size_t p_alphabet_dimension =
        compact_sequence_range<p_source>::traits::alphabet_size_n>
using scoring_matrix = scoring_matrix_storage<
    typename compact_sequence_range<p_source>::traits::alphabet_type,
    p_alphabet_dimension>;

static_assert(std::is_same_v<
    scoring_matrix<seq::dna4>,
    scoring_matrix<sequence_custom_alphabet<seq::dna4, std::uint64_t>>>);
static_assert(scoring_matrix<seq::dna4>::k_alphabet_dimension == 5u);
static_assert(scoring_matrix<seq::dna5>::k_alphabet_dimension == 6u);

/** Score and exact edit count for one non-overlapping implicit seed interval. */
struct implicit_seed_alignment_summary {
    std::int32_t score{};
    std::uint64_t edit_distance{};
};

/**
 * Replay a selected hash-candidate seed using bounded native-rank reads and the selected SIMD
 * equality kernel. Intervals use the selected query orientation and forward
 * target coordinates. The caller's counter validates ranks and reports equality;
 * mismatches remain ordinary substitutions within the implicit M run.
 * Dense matrices score actual target-row/query-column pairs; unequal tails use the cheaper affine
 * gap pair. No whole-sequence rank copy or source-owned pointer survives a read.
 */
template<bool p_query_reversed, class Query, class Target,
         sequence_alphabet Alphabet, std::size_t Dimension, class Counter>
    requires requires(const Query& query, const Target& target) {
        query.template borrow_rank_source<p_query_reversed>();
        target.template borrow_rank_source<false>();
    }
[[nodiscard]] inline std::expected<implicit_seed_alignment_summary,
                                  alignment_contract_error>
replay_implicit_seed_alignment(
    const Query& query, const Target& target,
    std::size_t query_start, std::size_t query_end,
    std::size_t target_start, std::size_t target_end,
    const scoring_matrix_storage<Alphabet, Dimension>& matrix,
    std::int32_t gap_open, std::int32_t gap_extend,
    std::int32_t gap_open_long, std::int32_t gap_extend_long,
    Counter counter) {
    static_assert(std::is_same_v<typename Query::traits::alphabet_type, Alphabet>);
    static_assert(std::is_same_v<typename Target::traits::alphabet_type, Alphabet>);
    if (query_start > query_end || query_end > query.bp_size()
        || target_start > target_end || target_end > target.bp_size()) {
        return std::unexpected(alignment_contract_error::coordinates);
    }
    if (!matrix.is_block_layout_valid()) {
        return std::unexpected(alignment_contract_error::invalid_matrix_layout);
    }
    const std::size_t query_length = query_end - query_start;
    const std::size_t target_length = target_end - target_start;
    const auto numeric = validate_alignment_numeric_contract(matrix.summary(),
        query_length, target_length, gap_open, gap_extend, 0, -1,
        gap_open_long, gap_extend_long, false);
    if (!numeric) return std::unexpected(numeric.error());

    const auto query_source = query.template borrow_rank_source<p_query_reversed>();
    const auto target_source = target.template borrow_rank_source<false>();
    constexpr std::size_t chunk_capacity = sequence_rank_io_chunk_capacity;
    std::array<std::uint8_t, chunk_capacity> query_chunk;
    std::array<std::uint8_t, chunk_capacity> target_chunk;
    const std::size_t common_length = std::min(query_length, target_length);
    std::int64_t score = 0;
    std::uint64_t edit_distance = 0;
    for (std::size_t consumed = 0; consumed < common_length;) {
        const std::size_t count = std::min(chunk_capacity, common_length - consumed);
        const auto query_ranks = query_source.read_checked(
            query_start + consumed, count, query_chunk);
        if (!query_ranks) return std::unexpected(alignment_contract_error::source_read_failed);
        // A second read may evict the first source's two-slot cache, including
        // when both operands alias one source. Retain only this bounded chunk.
        if (query_ranks->data() != query_chunk.data()) {
            std::copy(query_ranks->begin(), query_ranks->end(), query_chunk.begin());
        }
        const auto target_ranks = target_source.read_checked(
            target_start + consumed, count, target_chunk);
        if (!target_ranks) return std::unexpected(alignment_contract_error::source_read_failed);
        const auto compared = counter(query_chunk.data(), target_ranks->data(), count);
        if (!compared.valid_ranks || compared.equal_symbols > count) {
            return std::unexpected(alignment_contract_error::trace_accounting);
        }
        const auto mismatches = count - compared.equal_symbols;
        edit_distance += mismatches;
        if (matrix.summary().uniform_diagonal && matrix.summary().uniform_off_diagonal) {
            score += static_cast<std::int64_t>(compared.equal_symbols) * matrix.summary().equal_score
                + static_cast<std::int64_t>(mismatches) * matrix.summary().unequal_score;
        } else {
            // The selected candidate can contain substitutions, including
            // asymmetric target/query scores from the prepared matrix.
            for (std::size_t index = 0; index < count; ++index) {
                score += matrix.at((*target_ranks)[index], query_chunk[index]);
            }
        }
        consumed += count;
    }
    const std::size_t tail = std::max(query_length, target_length) - common_length;
    if (tail != 0u) {
        const std::int64_t first_cost = gap_open
            + static_cast<std::int64_t>(tail) * gap_extend;
        const std::int64_t second_cost = gap_open_long < 0 ? first_cost
            : gap_open_long + static_cast<std::int64_t>(tail) * gap_extend_long;
        score -= std::min(first_cost, second_cost);
    }
    if (score < std::numeric_limits<std::int32_t>::min()
        || score > std::numeric_limits<std::int32_t>::max()) {
        return std::unexpected(alignment_contract_error::score_accumulation);
    }
    return implicit_seed_alignment_summary{
        .score = static_cast<std::int32_t>(score), .edit_distance = edit_distance + tail};
}

}  // namespace bioseq::simd
