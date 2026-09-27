/**
 * @file bioseq_caching/core/coverage.cppm
 * @brief Assigns result rows to cached sources or new computation while preserving duplicates.
 */
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

export module bioseq.cache.core:coverage;
import :wire;

export namespace bioseq::cache {

enum class sql_bool : std::uint8_t {
    false_value = 0,
    true_value = 1,
    unknown = 2,
};

/** Reports whether a SQL condition passes WHERE. False and unknon are rejected. */
[[nodiscard]] constexpr bool passes_where(sql_bool value) noexcept {
    return value == sql_bool::true_value;
}

/** Model PostgreSQL `value IS NOT TRUE`. */
[[nodiscard]] constexpr sql_bool is_not_true(sql_bool value) noexcept {
    return value == sql_bool::true_value ? sql_bool::false_value : sql_bool::true_value;
}

/** One row occurrence with its requested-query and cached-source predicate results. */
struct coverage_row {
    std::uint64_t occurrence = 0;
    std::int64_t visible_value = 0;
    sql_bool requested_predicate = sql_bool::false_value;
    std::vector<sql_bool> source_predicates;
};

/** Records which row occurrences come from each cached source and which need fresh computation. */
class coverage_assignment {
  public:
    explicit coverage_assignment(std::size_t source_count) : branches_(source_count) {}

    /** Assigns a row occurrence to the  chosen cached source. */
    void append_branch(std::size_t source, std::uint64_t occurrence) {
        branches_[source].push_back(occurrence);
    }

    /** Adds a row occurrence that needs fresh computation. */
    void append_residual(std::uint64_t occurrence) {
        residual_.push_back(occurrence);
    }

    /** Return one cached branch's occurrence identifiers. */
    [[nodiscard]] const std::vector<std::uint64_t>& branch(std::size_t source) const noexcept {
        return branches_[source];
    }

    /** Returns the row occurrences that need fresh computation. */
    [[nodiscard]] const std::vector<std::uint64_t>& residual() const noexcept {
        return residual_;
    }

    /** Counts row occurrences across all branches, preserving duplicate values. */
    [[nodiscard]] std::size_t occurrence_count() const noexcept {
        std::size_t count = residual_.size();
        for (const auto& branch_rows : branches_) {
            count += branch_rows.size();
        }
        return count;
    }

    /** Returns all row-occurrence IDs in ascending order. */
    [[nodiscard]] std::vector<std::uint64_t> all_occurrences() const {
        std::vector<std::uint64_t> occurrences;
        occurrences.reserve(occurrence_count());
        for (const auto& branch_rows : branches_) {
            occurrences.insert(occurrences.end(), branch_rows.begin(), branch_rows.end());
        }
        occurrences.insert(occurrences.end(), residual_.begin(), residual_.end());
        std::sort(occurrences.begin(), occurrences.end());
        return occurrences;
    }

  private:
    std::vector<std::vector<std::uint64_t>> branches_;
    std::vector<std::uint64_t> residual_;
};

/**
 * Assigns each requested row to the first valid cached source whose
 * predicate is true. Rows that no valid source covers need fresh
 * computation. SQL unknown values fail the predicate check, and equal
 * row values remain separate occurrences.
 */
[[nodiscard]] inline coverage_assignment partition_coverage(std::span<const coverage_row> rows,
                                                            std::span<const bool> source_validity) {
    coverage_assignment result(source_validity.size());
    for (const coverage_row& row : rows) {
        if (!passes_where(row.requested_predicate)) {
            continue;
        }
        bool assigned = false;
        for (std::size_t source = 0; source < source_validity.size(); ++source) {
            if (!source_validity[source] || source >= row.source_predicates.size() ||
                !passes_where(row.source_predicates[source])) {
                continue;
            }
            bool excluded_by_earlier = false;
            for (std::size_t earlier = 0; earlier < source; ++earlier) {
                if (source_validity[earlier] && earlier < row.source_predicates.size() &&
                    passes_where(row.source_predicates[earlier])) {
                    excluded_by_earlier = true;
                    break;
                }
            }
            if (!excluded_by_earlier) {
                result.append_branch(source, row.occurrence);
                assigned = true;
                break;
            }
        }
        if (!assigned) {
            result.append_residual(row.occurrence);
        }
    }
    return result;
}

/** Values used to order cached sources before choosing  which rows each supplies. */
struct coverage_source_order_key {
    bool complete = false;
    std::uint64_t estimated_covered_rows = 0;
    std::uint64_t stored_bytes = 0;
    std::uint64_t claim_sequence = 0;
    cache_digest_256 digest{};
};

/**
 * Compares nonnegative fractions without multiplying their numerators
 * and denominators. Repeated division avoids overflow. A zero
 * denominator sorts after any finite fraction.
 */
[[nodiscard]] constexpr bool fraction_less_exact(std::uint64_t left_numerator,
                                                 std::uint64_t left_denominator,
                                                 std::uint64_t right_numerator,
                                                 std::uint64_t right_denominator) noexcept {
    if (left_denominator == 0 || right_denominator == 0) {
        return left_denominator != 0 && right_denominator == 0;
    }
    bool reversed = false;
    for (;;) {
        const std::uint64_t left_quotient = left_numerator / left_denominator;
        const std::uint64_t right_quotient = right_numerator / right_denominator;
        if (left_quotient != right_quotient) {
            return reversed ? left_quotient > right_quotient : left_quotient < right_quotient;
        }
        left_numerator %= left_denominator;
        right_numerator %= right_denominator;
        if (left_numerator == 0 || right_numerator == 0) {
            if (left_numerator == 0 && right_numerator == 0) {
                return false;
            }
            return reversed ? right_numerator == 0 : left_numerator == 0;
        }
        std::swap(left_numerator, left_denominator);
        std::swap(right_numerator, right_denominator);
        reversed = !reversed;
    }
}

/**
 * Prefers sources covering more rows, then completed sources and lower
 * bytes per covered row Claim sequence and digest break remaining ties.
 */
[[nodiscard]] inline bool
coverage_source_precedes(const coverage_source_order_key& left,
                         const coverage_source_order_key& right) noexcept {
    if (left.estimated_covered_rows != right.estimated_covered_rows) {
        return left.estimated_covered_rows > right.estimated_covered_rows;
    }
    if (left.complete != right.complete) {
        return left.complete;
    }
    const auto density_less = [](const coverage_source_order_key& first,
                                 const coverage_source_order_key& second) {
        if (first.estimated_covered_rows == 0) {
            return false;
        }
        if (second.estimated_covered_rows == 0) {
            return true;
        }
        return fraction_less_exact(first.stored_bytes,
                                   first.estimated_covered_rows,
                                   second.stored_bytes,
                                   second.estimated_covered_rows);
    };
    if (density_less(left, right)) {
        return true;
    }
    if (density_less(right, left)) {
        return false;
    }
    if (left.claim_sequence != right.claim_sequence) {
        return left.claim_sequence < right.claim_sequence;
    }
    return left.digest.bytes < right.digest.bytes;
}

} // namespace bioseq::cache
