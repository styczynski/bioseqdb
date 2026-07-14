/**
 * @file bioseq_caching/core/coverage.cppm
 * @brief SQL three-valued, bag-preserving coverage partition model.
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

enum class SqlBool : std::uint8_t {
    FalseValue = 0,
    TrueValue = 1,
    Unknown = 2,
};

/** Model PostgreSQL WHERE acceptance: only TRUE passes. */
[[nodiscard]] constexpr bool passes_where(SqlBool value) noexcept {
    return value == SqlBool::TrueValue;
}

/** Model PostgreSQL `value IS NOT TRUE`. */
[[nodiscard]] constexpr SqlBool is_not_true(SqlBool value) noexcept {
    return value == SqlBool::TrueValue
        ? SqlBool::FalseValue
        : SqlBool::TrueValue;
}

/** One expensive-domain occurrence and its predicate truth values. */
struct CoverageRow {
    std::uint64_t occurrence = 0;
    std::int64_t visible_value = 0;
    SqlBool requested_predicate = SqlBool::FalseValue;
    std::vector<SqlBool> source_predicates;
};

/** Disjoint branch ownership plus the newly computed residual occurrences. */
class CoverageAssignment {
public:
    explicit CoverageAssignment(std::size_t source_count)
        : branches_(source_count) {}

    /** Add one occurrence to its deterministic cached branch. */
    void append_branch(std::size_t source, std::uint64_t occurrence) {
        branches_[source].push_back(occurrence);
    }

    /** Add one occurrence to the residual expensive-domain branch. */
    void append_residual(std::uint64_t occurrence) {
        residual_.push_back(occurrence);
    }

    /** Return one cached branch's occurrence identifiers. */
    [[nodiscard]] const std::vector<std::uint64_t>& branch(
        std::size_t source) const noexcept {
        return branches_[source];
    }

    /** Return residual occurrence identifiers. */
    [[nodiscard]] const std::vector<std::uint64_t>& residual() const noexcept {
        return residual_;
    }

    /** Count every retained occurrence without value-based deduplication. */
    [[nodiscard]] std::size_t occurrence_count() const noexcept {
        std::size_t count = residual_.size();
        for (const auto& branch_rows : branches_) {
            count += branch_rows.size();
        }
        return count;
    }

    /** Return all occurrence IDs in ascending proof-fixture order. */
    [[nodiscard]] std::vector<std::uint64_t> all_occurrences() const {
        std::vector<std::uint64_t> occurrences;
        occurrences.reserve(occurrence_count());
        for (const auto& branch_rows : branches_) {
            occurrences.insert(
                occurrences.end(), branch_rows.begin(), branch_rows.end());
        }
        occurrences.insert(
            occurrences.end(), residual_.begin(), residual_.end());
        std::sort(occurrences.begin(), occurrences.end());
        return occurrences;
    }

private:
    std::vector<std::vector<std::uint64_t>> branches_;
    std::vector<std::uint64_t> residual_;
};

/**
 * Apply the normative disjoint formula. Invalid earlier sources do not
 * exclude overlap from later valid sources or the residual.
 */
[[nodiscard]] inline CoverageAssignment partition_coverage(
    std::span<const CoverageRow> rows,
    std::span<const bool> source_validity) {
    CoverageAssignment result(source_validity.size());
    for (const CoverageRow& row : rows) {
        if (!passes_where(row.requested_predicate)) {
            continue;
        }
        bool assigned = false;
        for (std::size_t source = 0;
             source < source_validity.size();
             ++source) {
            if (!source_validity[source]
                || source >= row.source_predicates.size()
                || !passes_where(row.source_predicates[source])) {
                continue;
            }
            bool excluded_by_earlier = false;
            for (std::size_t earlier = 0; earlier < source; ++earlier) {
                if (source_validity[earlier]
                    && earlier < row.source_predicates.size()
                    && passes_where(row.source_predicates[earlier])) {
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

/** Deterministic source-order inputs from the registry candidate model. */
struct CoverageSourceOrderKey {
    bool complete = false;
    std::uint64_t estimated_covered_rows = 0;
    std::uint64_t stored_bytes = 0;
    std::uint64_t claim_sequence = 0;
    CacheDigest256 digest{};
};

/** Compare sources using completion, benefit, density, sequence, then digest. */
[[nodiscard]] inline bool coverage_source_precedes(
    const CoverageSourceOrderKey& left,
    const CoverageSourceOrderKey& right) noexcept {
    if (left.complete != right.complete) {
        return left.complete;
    }
    if (left.estimated_covered_rows != right.estimated_covered_rows) {
        return left.estimated_covered_rows > right.estimated_covered_rows;
    }
    const auto density_less = [](const CoverageSourceOrderKey& first,
                                 const CoverageSourceOrderKey& second) {
        if (first.estimated_covered_rows == 0) {
            return false;
        }
        if (second.estimated_covered_rows == 0) {
            return true;
        }
        const long double first_density =
            static_cast<long double>(first.stored_bytes)
            / static_cast<long double>(first.estimated_covered_rows);
        const long double second_density =
            static_cast<long double>(second.stored_bytes)
            / static_cast<long double>(second.estimated_covered_rows);
        return first_density < second_density;
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
