/**
 * @file sneaky_snake_test.cpp
 * @brief Check SneakySnake lower bounds and agreement between scalar, Highway, and Vulkan results.
 *
 * Independent edit-distance tests check that  valid matches survive. Other
 * cases cover chunk boundaries, inactive SIMD lanes, invalid slices, and
 * Vulkan command replay after input bytes change.
 */
#include <hwy/highway.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.sequences;
import bioseq.common.seqan_adapter;
import bioseq.prefilter.types;
import bioseq.prefilter.sneaky_snake;
import bioseq.prefilter.sneaky_snake.vulkan;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.gpu.vulkan.abi;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;
using namespace bioseq::prefilter;

namespace {
/** Keeps borrowed byte-fixture descriptor state alive for one synchronous batch. */
struct rank_batch {
    std::span<const std::uint8_t> query_ranks;
    std::span<const std::uint8_t> target_ranks;
    std::span<const prefilter_job> jobs;
    /** Test-only native aa27 rank source; poison outside selected intervals remains unread. */
    static const std::uint8_t* read(const void* state, bioseq::storage::sequence_size_t from,
                                  std::size_t, std::span<std::uint8_t>) {
        return static_cast<const std::span<const std::uint8_t>*>(state)->data() + from;
    }
    std::array<bioseq::storage::borrowed_rank_window_source, 1> queries{{{
        .context = &query_ranks, .read_at = &read, .symbol_count = query_ranks.size(),
        .alphabet_code = supported_biological_alphabets::index_of<seq::aa27>(),
        .worker_shareable = true}}};
    std::array<bioseq::storage::borrowed_rank_window_source, 1> targets{{{
        .context = &target_ranks, .read_at = &read, .symbol_count = target_ranks.size(),
        .alphabet_code = supported_biological_alphabets::index_of<seq::aa27>(),
        .worker_shareable = true}}};
    /** Borrows descriptor arrays, preserving the fixture's original job order. */
    operator prefilter_batch_input() const { return {queries, targets, jobs}; }
};

/** Requires successful checked input before comparing numerical results. */
template <class Value, class Error>
[[nodiscard]] Value checked_value(std::expected<Value, Error> result) {
    REQUIRE(result.has_value());
    return std::move(*result);
}


/** Prepares native packed aa27 input outside the device call, including fixture guard symbols. */
[[nodiscard]] compact_sequence_range<seq::aa27> packed_ranks(std::span<const std::uint8_t> ranks) {
    std::vector<seq::aa27> letters;
    letters.reserve(ranks.size());
    for (const auto rank : ranks) {
        seq::aa27 letter{};
        letter.assign_rank(rank % seqan3::alphabet_size<seq::aa27>);
        letters.push_back(letter);
    }
    return compact_sequence_range<seq::aa27>{letters};
}

/**
 * @brief Get the SneakySnake kernel for this test's Highway target.
 * @return The generated table's callable.
 * @throws std::logic_error If the table has no SneakySnake kernel.
 */
[[nodiscard]] ::bioseq::simd_dispatch::sneaky_snake_simd_batch_kernel
fixed_sneaky_snake_kernel()
{
    const auto kernel = ::bioseq::simd_dispatch::highway_test_kernel_table()
        .common.sneaky_snake_simd_batch;
    if (kernel == nullptr) {
        throw std::logic_error{
            "generated fixed-target test table lacks SneakySnake"};
    }
    return kernel;
}

/**
 * @brief Compute exact edit distance independently for the filter tests.
 *
 * Insertions, deletions, and substitutions cost one The calculation keeps
 * the current and previous rows of the dynamic-programming matrix.
 * @param[in] query Query ranks in sequence order.
 * @param[in] target Target ranks in sequence  order.
 * @return Global Levenshtein distance.
 */
[[nodiscard]] std::uint32_t exact_levenshtein(
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target)
{
    std::vector<std::uint32_t> previous(target.size() + 1u);
    std::vector<std::uint32_t> current(target.size() + 1u);
    for (std::size_t column = 0u; column <= target.size(); ++column) {
        previous[column] = static_cast<std::uint32_t>(column);
    }

    for (std::size_t row = 1u; row <= query.size(); ++row) {
        current[0] = static_cast<std::uint32_t>(row);
        for (std::size_t column = 1u; column <= target.size(); ++column) {
            const std::uint32_t substitution = previous[column - 1u]
                + (query[row - 1u] == target[column - 1u] ? 0u : 1u);
            current[column] = std::min({
                static_cast<std::uint32_t>(previous[column] + 1u),
                static_cast<std::uint32_t>(current[column - 1u] + 1u),
                substitution,
            });
        }
        previous.swap(current);
    }
    return previous.back();
}

/**
 * @brief Compare the lower bound, status, acceptance flag, and diagnostics.
 * @return True when all four fields match.
 */
[[nodiscard]] bool same_result(
    const prefilter_result& lhs,
    const prefilter_result& rhs) noexcept
{
    return lhs.lower_bound == rhs.lower_bound
        && lhs.status == rhs.status
        && lhs.is_accepted == rhs.is_accepted
        && lhs.diagnostics == rhs.diagnostics;
}

/**
 * @brief Generate every binary sequence up to max_length, including the empty sequence.
 * @return Sequences ordered by length, then by integer bit pattern aith the lowest bit first.
 */
[[nodiscard]] std::vector<std::vector<std::uint8_t>> binary_sequences(
    std::uint32_t max_length)
{
    std::vector<std::vector<std::uint8_t>> sequences;
    for (std::uint32_t length = 0u; length <= max_length; ++length) {
        const std::uint32_t count = 1u << length;
        for (std::uint32_t bits = 0u; bits < count; ++bits) {
            std::vector<std::uint8_t> sequence(length);
            for (std::uint32_t index = 0u; index < length; ++index) {
                sequence[index] = static_cast<std::uint8_t>(
                    (bits >> index) & 1u);
            }
            sequences.push_back(std::move(sequence));
        }
    }
    return sequences;
}

/** Reproducible rank generator for test inputs. */
class deterministic_ranks {
public:
    /** Advance the, generator and return the next 32-bit value. */
    [[nodiscard]] std::uint32_t next() noexcept
    {
        state_ = state_ * 1664525u + 1013904223u;
        return state_;
    }

private:
    std::uint32_t state_ = 0x5EED1234u;
};

/**
 * @brief Keep both  batch buffers and separate sequence copies for comparisons.
 *
 * Jobs address slices in query_ranks and target_ranks. The separate queries
 * and targets let the exact-distance reference check each pair.
 */
struct batch_fixture {
    /** Query bytes with extra ranks around each slice to expose incorrect offsets. */
    std::vector<std::uint8_t> query_ranks;
    /** Target bytes with extra ranks around each slice to expose incorrect offsets. */
    std::vector<std::uint8_t> target_ranks;
    /** Slice offsets, lengths, and edit limits for each query-target pair. */
    std::vector<prefilter_job> jobs;
    /** Separate query sequences for exact-distance checks. */
    std::vector<std::vector<std::uint8_t>> queries;
    /** Separate target sequences for exact-distance checks. */
    std::vector<std::vector<std::uint8_t>> targets;
};

/**
 * @brief Build a reproducible batch  with varied lengths, alphabets, and edit limits.
 *
 * The first cases cross the 512-base chunk boundary. Extra bytes surround each
 * slice to catch reads outside it
 * @param[in] job_count Number of query-target pairs.
 * @return Buffers, jobs, and separate copies of each sequence pair.
 */
[[nodiscard]] batch_fixture random_batch(std::size_t job_count)
{
    deterministic_ranks random;
    batch_fixture fixture;
    fixture.jobs.reserve(job_count);
    fixture.queries.reserve(job_count);
    fixture.targets.reserve(job_count);

    constexpr std::array<std::uint32_t, 4u> boundary_lengths{
        511u, 512u, 513u, 1025u};

    for (std::size_t index = 0u; index < job_count; ++index) {
        const std::uint32_t query_length = index < boundary_lengths.size()
            ? boundary_lengths[index]
            : random.next() % 81u;
        const bool equal_length = index < boundary_lengths.size()
            || index % 4u != 0u;
        const std::uint32_t target_length = equal_length
            ? query_length
            : random.next() % 81u;
        const std::uint32_t alphabet_size = 2u + random.next() % (seqan3::alphabet_size<seq::aa27> - 1u);

        std::vector<std::uint8_t> query(query_length);
        std::vector<std::uint8_t> target(target_length);
        for (std::uint8_t& rank : query) {
            rank = static_cast<std::uint8_t>(random.next() % alphabet_size);
        }
        for (std::uint8_t& rank : target) {
            rank = static_cast<std::uint8_t>(random.next() % alphabet_size);
        }

        // Extra ranks around each slice make incorrect offsets visible in the result.
        fixture.query_ranks.push_back(255u);
        const std::uint32_t query_offset = static_cast<std::uint32_t>(
            fixture.query_ranks.size());
        fixture.query_ranks.insert(
            fixture.query_ranks.end(), query.begin(), query.end());
        fixture.query_ranks.push_back(254u);

        fixture.target_ranks.push_back(253u);
        const std::uint32_t target_offset = static_cast<std::uint32_t>(
            fixture.target_ranks.size());
        fixture.target_ranks.insert(
            fixture.target_ranks.end(), target.begin(), target.end());
        fixture.target_ranks.push_back(252u);

        fixture.jobs.push_back(prefilter_job{
            .query_start = query_offset,
            .target_start = target_offset,
            .query_length = query_length,
            .target_length = target_length,
            .max_edits = random.next() % 17u,
        });
        fixture.queries.push_back(std::move(query));
        fixture.targets.push_back(std::move(target));
    }
    return fixture;
}

TEST_CASE(
    "SneakySnake stable ABI and canonical fixtures",
    "[component:bioseq][layer:unit][feature:prefilter]")
{
    STATIC_REQUIRE(k_sneaky_snake_default_submaze_width == 512u);
    STATIC_REQUIRE(k_sneaky_snake_default_max_escape_segments == 512u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(prefilter_status::success) == 0u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
        prefilter_status::conservative_unequal) == 1u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
        prefilter_status::unsupported_conservative) == 2u);
    STATIC_REQUIRE(sizeof(prefilter_job) == 32u);
    STATIC_REQUIRE(sizeof(prefilter_result) == 16u);

    const std::array<std::uint8_t, 8u> identical{
        0u, 4u, 17u, 255u, 9u, 9u, 2u, 41u};
    const auto identical_result = sneaky_snake_scalar(
        identical, identical, 0u);
    REQUIRE(identical_result.lower_bound == 0u);
    REQUIRE(is_prefilter_evaluated(identical_result));
    REQUIRE(is_prefilter_accepted(identical_result));

    auto substituted = identical;
    substituted[3] = 128u;
    const auto rejected = sneaky_snake_scalar(
        identical, substituted, 0u);
    REQUIRE(rejected.lower_bound == 1u);
    REQUIRE(is_prefilter_evaluated(rejected));
    REQUIRE_FALSE(is_prefilter_accepted(rejected));

    const auto accepted = sneaky_snake_scalar(
        identical, substituted, 1u);
    REQUIRE(accepted.lower_bound == 1u);
    REQUIRE(is_prefilter_accepted(accepted));

    // A one-rank shift reaches a matching off-diagonal after one obstcle.
    // These cases check both shift directions and expect a lower bound of one.
    const std::array<std::uint8_t, 6u> source_row{0u, 1u, 2u, 3u, 4u, 5u};
    const std::array<std::uint8_t, 6u> shifted_right{9u, 0u, 1u, 2u, 3u, 4u};
    const std::array<std::uint8_t, 6u> shifted_left{1u, 2u, 3u, 4u, 5u, 9u};
    const auto upper_diagonal = sneaky_snake_scalar(
        source_row, shifted_right, 1u);
    const auto lower_diagonal = sneaky_snake_scalar(
        source_row, shifted_left, 1u);
    REQUIRE(upper_diagonal.lower_bound == 1u);
    REQUIRE(lower_diagonal.lower_bound == 1u);
    REQUIRE(is_prefilter_accepted(upper_diagonal));
    REQUIRE(is_prefilter_accepted(lower_diagonal));

    const std::array<std::uint8_t, 3u> shorter{0u, 1u, 2u};
    const std::array<std::uint8_t, 6u> longer{0u, 1u, 2u, 3u, 4u, 5u};
    const auto unequal_reject = sneaky_snake_scalar(shorter, longer, 2u);
    REQUIRE(unequal_reject.lower_bound == 3u);
    REQUIRE_FALSE(is_prefilter_evaluated(unequal_reject));
    REQUIRE_FALSE(is_prefilter_accepted(unequal_reject));
    const auto unequal_accept = sneaky_snake_scalar(shorter, longer, 3u);
    REQUIRE(unequal_accept.lower_bound == 3u);
    REQUIRE_FALSE(is_prefilter_evaluated(unequal_accept));
    REQUIRE(is_prefilter_accepted(unequal_accept));
}

TEST_CASE(
    "SneakySnake includes the production submaze tail and honors escape caps",
    "[component:bioseq][layer:unit][feature:prefilter]")
{
    std::vector<std::uint8_t> query(513u, 0u);
    std::vector<std::uint8_t> target(513u, 0u);
    target.front() = 1u;
    target.back() = 1u;

    const auto tail_result = sneaky_snake_scalar(query, target, 2u);
    REQUIRE(tail_result.lower_bound == 2u);
    REQUIRE(is_prefilter_accepted(tail_result));

    query.assign(20u, 0u);
    target.assign(20u, 255u);
    const sneaky_snake_options capped{
        .submaze_width = 8u,
        .max_escape_segments = 1u,
    };
    const auto capped_result = sneaky_snake_scalar(query, target, 10u, capped);
    REQUIRE(capped_result.lower_bound == 3u);
    REQUIRE(capped_result.lower_bound <= exact_levenshtein(query, target));
    REQUIRE(is_prefilter_accepted(capped_result));
}

TEST_CASE(
    "SneakySnake lower bound is exhaustive-sound on short binary strings",
    "[component:bioseq][layer:unit][feature:prefilter]")
{
    const auto sequences = binary_sequences(6u);
    for (const auto& query : sequences) {
        for (const auto& target : sequences) {
            const std::uint32_t distance = exact_levenshtein(query, target);
            const std::uint32_t length_difference = query.size() > target.size()
                ? static_cast<std::uint32_t>(query.size() - target.size())
                : static_cast<std::uint32_t>(target.size() - query.size());
            for (std::uint32_t max_edits = 0u; max_edits <= 7u; ++max_edits) {
                const prefilter_result result = sneaky_snake_scalar(
                    query, target, max_edits);
                CAPTURE(query, target, distance, max_edits, result.lower_bound);
                REQUIRE(result.lower_bound <= distance);
                REQUIRE(result.diagnostics == 0u);
                if (query.size() == target.size()) {
                    REQUIRE(is_prefilter_evaluated(result));
                    REQUIRE(is_prefilter_accepted(result)
                            == (result.lower_bound <= max_edits));
                } else {
                    REQUIRE_FALSE(is_prefilter_evaluated(result));
                    REQUIRE(result.lower_bound == length_difference);
                    REQUIRE(is_prefilter_accepted(result)
                            == (length_difference <= max_edits));
                }
                if (distance <= max_edits) {
                    REQUIRE(is_prefilter_accepted(result));
                }
            }
        }
    }
}

// BEGIN_TEMPORARY_NATIVE_SNEAKY_PROBE -- DO NOT COMMIT
namespace manual_native_sneaky_probe {
namespace storage = bioseq::storage;
constexpr std::array<std::size_t, 6u> boundaries{63u, 64u, 65u, 511u, 512u, 513u};
constexpr std::size_t query_start = 2u;
constexpr std::size_t target_start = 3u;

/** Count optional reads without making them a requirement of a resident provider. */
template<sequence_alphabet Alphabet>
struct source_probe {
    using view_type = storage::lazy_sequence_view<Alphabet>;
    view_type parent;
    const std::vector<std::uint8_t>& ranks;
    std::size_t leading;
    mutable std::size_t reads{};
    bool allow_resident{true};
    bool fail_reads{};
    std::array<std::uint8_t, prefilter_rank_window::chunk_size>* shared_slot{};
    std::vector<bool>* read_order{};
    bool target_side{};

    /** The independent rank oracle also supplies checked-reader failure diagnosis. */
    template<bool Reverse, bool WriteReverse>
    [[nodiscard]] static const std::uint8_t* read(
        const void* context, storage::sequence_size_t from, std::size_t count,
        std::span<std::uint8_t> scratch) {
        const auto& self = *static_cast<const source_probe*>(context);
        ++self.reads;
        if (self.read_order != nullptr) self.read_order->push_back(self.target_side);
        if (self.fail_reads || from > self.ranks.size() ||
            count > self.ranks.size() - from || count > scratch.size()) return nullptr;
        if constexpr (Reverse && !seqan3::nucleotide_alphabet<Alphabet>) return nullptr;
        else {
            auto output = self.shared_slot == nullptr ? scratch
                : std::span<std::uint8_t>{*self.shared_slot};
            if (count > output.size()) return nullptr;
            for (std::size_t index = 0u; index < count; ++index) {
                const auto offset = WriteReverse ? count - 1u - index : index;
                auto rank = self.ranks[Reverse ? self.ranks.size() - 1u - from - offset : from + offset];
                if constexpr (Reverse) {
                    if (rank >= seqan3::alphabet_size<Alphabet>) return nullptr;
                    rank = seqan3::to_rank(seqan3::complement(seqan3::assign_rank_to(rank, Alphabet{})));
                }
                output[index] = rank;
            }
            return output.data();
        }
    }
    /** Providers with mandatory failure or evictable storage explicitly decline residency. */
    [[nodiscard]] static typename view_type::resident_packed_result resident(
        const void* context, storage::sequence_size_t from, storage::sequence_size_t to,
        std::uint8_t unit_bytes) noexcept {
        const auto& self = *static_cast<const source_probe*>(context);
        if (!self.allow_resident || self.fail_reads || self.shared_slot != nullptr ||
            from > to || to > self.ranks.size()) return std::nullopt;
        return self.parent.try_resident_packed(self.leading + from, self.leading + to, unit_bytes);
    }
    /** Bind existing lazy-view APIs; all backing owners outlive each synchronous batch. */
    [[nodiscard]] view_type view() const noexcept {
        static const typename view_type::backend_ops operations{
            .bp_size = +[](const void* context) noexcept -> storage::sequence_size_t {
                return static_cast<const source_probe*>(context)->ranks.size();
            },
            .read_at = {read<false, false>, read<false, true>, read<true, false>, read<true, true>},
            .try_resident_packed = resident, .copy_packed_sequence = nullptr, .copy_packed_window = nullptr,
            .worker_shareable = +[](const void*) noexcept { return false; }};
        return view_type{this, &operations};
    }
};

/** Own native words and a separate logical oracle; never move a bound provider. */
template<sequence_alphabet Alphabet, class Word>
struct native_fixture {
    using range_type = compact_sequence_range<sequence_custom_alphabet<Alphabet, Word>>;
    using traits = typename range_type::traits;
    std::vector<std::uint8_t> ranks;
    range_type packed;
    source_probe<Alphabet> source;

    /** Include a prefix and suffix so boundary slots are genuinely outside the borrowed region. */
    static range_type pack(std::span<const std::uint8_t> input, std::size_t leading, bool exact_extent) {
        std::vector<Alphabet> letters(leading + input.size()
            + (exact_extent ? 0u : traits::letters_per_unit), Alphabet{});
        for (std::size_t index = 0u; index < input.size(); ++index) {
            REQUIRE(input[index] < traits::alphabet_size);
            letters[leading + index] = seqan3::assign_rank_to(input[index], Alphabet{});
        }
        return range_type{letters};
    }
    native_fixture(std::vector<std::uint8_t> input, std::size_t leading, bool exact_extent = false)
        : ranks(std::move(input)), packed(pack(ranks, leading, exact_extent)),
          source{storage::make_lazy_sequence_view(packed), ranks, leading} {
        // Poison every unobserved symbol and unused high bit; active symbols retain their ranks.
        for (std::size_t unit = 0u; unit < packed.packed_units.size(); ++unit) {
            Word active{};
            for (std::size_t slot = 0u; slot < traits::letters_per_unit; ++slot) {
                const auto position = unit * traits::letters_per_unit + slot;
                if (position >= leading && position - leading < ranks.size())
                    active |= static_cast<Word>(static_cast<std::uint64_t>(traits::char_mask)
                        << (slot * traits::bits_per_char));
            }
            packed.packed_units[unit] |= static_cast<Word>(~active);
        }
    }
    native_fixture(const native_fixture&) = delete;
    native_fixture(native_fixture&&) = delete;
    /** Corrupt both representations before any resident descriptor is bound. */
    void corrupt(std::size_t logical_position) {
        REQUIRE(traits::alphabet_size <= traits::char_mask);
        const auto position = source.leading + logical_position;
        const auto shift = (position % traits::letters_per_unit) * traits::bits_per_char;
        const auto mask = static_cast<Word>(static_cast<std::uint64_t>(traits::char_mask) << shift);
        const auto invalid = static_cast<Word>(static_cast<std::uint64_t>(traits::alphabet_size) << shift);
        auto& unit = packed.packed_units[position / traits::letters_per_unit];
        unit = static_cast<Word>((unit & static_cast<Word>(~mask)) | invalid);
        ranks[logical_position] = static_cast<std::uint8_t>(traits::alphabet_size);
    }
};

/** One numeric job with nonzero coordinates in both independently borrowed sources. */
[[nodiscard]] prefilter_job job_for(std::size_t length, std::uint32_t edits, bool reverse = false) {
    return {.query_start = query_start, .target_start = target_start,
        .query_length = static_cast<std::uint32_t>(length), .target_length = static_cast<std::uint32_t>(length),
        .max_edits = edits, .orientation = static_cast<std::uint32_t>(reverse
            ? storage::sequence_transform::reverse_complement : storage::sequence_transform::identity)};
}

/** Select the same scalar/Highway entry points used by ordinary callers. */
[[nodiscard]] prefilter_batch_result run_backend(bool simd, const prefilter_batch_input& input,
                                                sneaky_snake_options options) {
    return simd ? fixed_sneaky_snake_kernel()(input, options) : sneaky_snake_scalar_batch(input, options);
}

/** Full result-tuple parity, paired with callback evidence of native versus byte execution. */
template<sequence_alphabet Alphabet>
void compare(source_probe<Alphabet>& query, source_probe<Alphabet>& target,
             std::span<const std::uint8_t> oriented_query, const prefilter_job& job,
             bool native_expected, bool reverse = false) {
    std::array<storage::borrowed_rank_window_source, 1u> queries;
    if constexpr (seqan3::nucleotide_alphabet<Alphabet>) {
        queries[0] = reverse ? query.view().template borrow_rank_source<true>(true)
                             : query.view().template borrow_rank_source<false>(true);
    } else {
        REQUIRE_FALSE(reverse);
        queries[0] = query.view().template borrow_rank_source<false>(true);
    }
    const std::array targets{target.view().template borrow_rank_source<false>(true)};
    if (native_expected) {
        REQUIRE(queries[0].resident.has_value());
        REQUIRE(targets[0].resident.has_value());
    }
    const std::array jobs{job};
    const prefilter_batch_input input{queries, targets, jobs};
    const sneaky_snake_options options{.submaze_width = job.query_length, .max_escape_segments = 0u};
    const auto oracle = sneaky_snake_scalar(oriented_query.subspan(job.query_start, job.query_length),
        std::span<const std::uint8_t>{target.ranks}.subspan(job.target_start, job.target_length), job.max_edits, options);
    for (const bool simd : {false, true}) {
        CAPTURE(simd, job.query_length, job.max_edits, reverse, native_expected);
        query.reads = target.reads = 0u;
        const auto actual = run_backend(simd, input, options);
        REQUIRE(actual.has_value());
        REQUIRE(actual->size() == 1u);
        REQUIRE(same_result(actual->front(), oracle));
        if (native_expected) {
            REQUIRE(query.reads == 0u);
            REQUIRE(target.reads == 0u);
        } else {
            REQUIRE(query.reads > 0u);
            REQUIRE(target.reads > 0u);
        }
    }
}

/** Compare typed first-invalid failures with the same checked byte-source contract. */
template<sequence_alphabet Alphabet>
void invalid_acquisition(source_probe<Alphabet>& query, source_probe<Alphabet>& target,
                         std::size_t length, bool target_failure, std::size_t expected_position) {
    const std::array jobs{job_for(length, static_cast<std::uint32_t>(length - 1u))};
    const sneaky_snake_options options{.submaze_width = static_cast<std::uint32_t>(length)};
    for (const bool simd : {false, true}) {
        for (const bool native : {false, true}) {
            const std::array queries{query.view().template borrow_rank_source<false>(native)};
            const std::array targets{target.view().template borrow_rank_source<false>(native)};
            query.reads = target.reads = 0u;
            const auto actual = run_backend(simd, {queries, targets, jobs}, options);
            REQUIRE_FALSE(actual.has_value());
            REQUIRE(actual.error().code == prefilter_input_failure_code::invalid_rank);
            REQUIRE(actual.error().job_index == 0u);
            REQUIRE(actual.error().target_source == target_failure);
            REQUIRE(actual.error().position == expected_position);
            if (target_failure) {
                REQUIRE(target.reads == 1u);
                REQUIRE(query.reads == 0u);
            } else {
                REQUIRE(query.reads > 0u);
                if (native) REQUIRE(target.reads == 0u);
            }
        }
    }
}

/** Exact-extent windows distinguish full native tiles from bounded fallbacks. */
template<sequence_alphabet Alphabet, class Word>
void bounded_primitive_profile() {
    using fixture = native_fixture<Alphabet, Word>;
    using traits = typename fixture::traits;
    constexpr auto letters = traits::letters_per_unit;
    constexpr auto group = sizeof(std::uint64_t) / sizeof(Word) * letters;
    const hn::ScalableTag<Word> descriptor;
    const auto vector_symbols = hn::Lanes(descriptor) * letters;
    // The ordinary test table is generated for this translation unit's static target.
    REQUIRE(bioseq::simd_dispatch::highway_test_target_binding().target == HWY_TARGET);
    const auto counter = bioseq::simd_dispatch::highway_test_kernel_table()
        .common.resolve_sequence_packed_equality(
            supported_biological_alphabets::template index_of<Alphabet>(), sizeof(Word), false);
    REQUIRE(static_cast<bool>(counter));
    const auto scalar_refill = sneaky_snake_detail::scalar_traversal::read_block_size();
    const auto simd_refill = 1u + 2u * hn::Lanes(hn::ScalableTag<std::uint8_t>{});
    std::vector<std::size_t> lengths{
        1u, group - 1u, group, group + 1u,
        vector_symbols - 1u, vector_symbols, vector_symbols + 1u,
        scalar_refill + group, simd_refill + vector_symbols};
    std::sort(lengths.begin(), lengths.end());
    lengths.erase(std::unique(lengths.begin(), lengths.end()), lengths.end());
    for (const auto length : lengths) {
        if (length == 0u) continue; // The retained provider controls already cover zero acquisition.
        REQUIRE(length <= std::numeric_limits<std::uint32_t>::max());
        std::vector<std::uint8_t> query(length);
        for (std::size_t index = 0u; index < length; ++index)
            query[index] = static_cast<std::uint8_t>((index * 7u + traits::alphabet_size - 1u)
                % traits::alphabet_size);
        // length means equal; other positions exercise immediate, later-word and final mismatches.
        std::vector<std::size_t> mutations{length, 0u, letters - 1u, letters, length - 1u};
        std::sort(mutations.begin(), mutations.end());
        mutations.erase(std::unique(mutations.begin(), mutations.end()), mutations.end());
        for (const auto mutation : mutations) {
            if (mutation > length) continue;
            auto target = query;
            if (mutation < length) target[mutation] = static_cast<std::uint8_t>(
                (target[mutation] + 1u) % traits::alphabet_size);
            // These owners have exactly the needed native objects, not the old probe's suffix.
            fixture q{query, letters - 1u, true}, t{target, 0u, true};
            REQUIRE(q.packed.packed_units.size() == (q.source.leading + length) / letters
                + static_cast<std::size_t>((q.source.leading + length) % letters != 0u));
            REQUIRE(t.packed.packed_units.size() == length / letters
                + static_cast<std::size_t>(length % letters != 0u));
            const prefilter_job job{
                .query_start = 0u, .target_start = 0u,
                .query_length = static_cast<std::uint32_t>(length),
                .target_length = static_cast<std::uint32_t>(length), .max_edits = 0u};
            CAPTURE(sizeof(Word), traits::alphabet_size, length, mutation, vector_symbols, group);
            compare(q.source, t.source, q.ranks, job, true);
            const auto compared = counter(q.packed.packed_units.data(), t.packed.packed_units.data(),
                q.source.leading + length, q.source.leading, 0u, length);
            REQUIRE(compared.valid_ranks);
            REQUIRE(compared.equal_symbols == length - static_cast<std::size_t>(mutation < length));
        }
    }
    // Derive the actual batch boundary from the target and the lane-sum capacity.
    constexpr auto maximum_words = std::numeric_limits<Word>::max() / letters;
    auto batch_words = hn::Lanes(descriptor);
    if constexpr (maximum_words < hn::MaxLanes(hn::ScalableTag<Word>{}))
        batch_words = std::min(batch_words, static_cast<std::size_t>(maximum_words));
    const auto batch_symbols = batch_words * letters;
    constexpr bool full_cardinality = traits::alphabet_size
        == static_cast<std::size_t>(traits::char_mask) + 1u;
    for (const auto length : std::array<std::size_t, 3u>{
             batch_symbols - 1u, batch_symbols, batch_symbols + 1u}) {
        std::vector<std::uint8_t> query(length, static_cast<std::uint8_t>(traits::alphabet_size - 1u));
        std::vector<std::uint8_t> target(length, static_cast<std::uint8_t>(traits::alphabet_size - 2u));
        fixture q{query, letters - 1u, true}, t{target, 0u, true};
        const auto unequal = counter(q.packed.packed_units.data(), t.packed.packed_units.data(),
            q.source.leading + length, q.source.leading, 0u, length);
        const auto equal = counter(q.packed.packed_units.data(), q.packed.packed_units.data(),
            q.source.leading + length, q.source.leading, q.source.leading, length);
        REQUIRE(unequal.valid_ranks);
        REQUIRE(equal.valid_ranks);
        REQUIRE(unequal.equal_symbols == 0u);
        REQUIRE(equal.equal_symbols == length);
        const bool short_equal = length == 0u || (full_cardinality && length <= 2u * letters);
        if (short_equal) REQUIRE(equal.vector_batches == 0u);
        else {
            REQUIRE(equal.vector_batches > 0u);
            REQUIRE(unequal.vector_batches == equal.vector_batches);
        }
        if (length != 0u) REQUIRE(unequal.vector_batches > 0u);
    }
    if constexpr (traits::alphabet_size <= traits::char_mask) {
        // Equal bad words must fail validation, including the final active rank of a partial tile.
        const auto length = vector_symbols + 1u;
        const std::vector<std::uint8_t> ranks(length,
            static_cast<std::uint8_t>(traits::alphabet_size - 1u));
        for (const auto bad_position : std::array<std::size_t, 2u>{0u, length - 1u}) {
            fixture bad{ranks, letters - 1u, true}, valid{ranks, 0u, true};
            bad.corrupt(bad_position); // Before either pointer is passed to the immutable input ABI.
            REQUIRE_FALSE(counter(bad.packed.packed_units.data(), bad.packed.packed_units.data(),
                bad.source.leading + length, bad.source.leading, bad.source.leading, length).valid_ranks);
            REQUIRE_FALSE(counter(bad.packed.packed_units.data(), valid.packed.packed_units.data(),
                bad.source.leading + length, bad.source.leading, 0u, length).valid_ranks);
            REQUIRE_FALSE(counter(valid.packed.packed_units.data(), bad.packed.packed_units.data(),
                length, 0u, bad.source.leading, length).valid_ranks);
        }
    }
}

/** Registry-layout matrix: native rank values, bounded leading/tails and acquisition boundaries. */
template<sequence_alphabet Alphabet, class Word>
void profile() {
    using fixture = native_fixture<Alphabet, Word>;
    using traits = typename fixture::traits;
    constexpr auto query_leading = traits::letters_per_unit > 1u ? 1u : 0u;
    constexpr auto target_leading = traits::letters_per_unit - 1u;
    CAPTURE(sizeof(Word), traits::alphabet_size, traits::letters_per_unit);
    bounded_primitive_profile<Alphabet, Word>();
    for (const auto length : boundaries) {
        for (unsigned pattern = 0u; pattern < 3u; ++pattern) {
            std::vector<std::uint8_t> query(length + query_start + 1u);
            std::vector<std::uint8_t> target(length + target_start + 1u);
            for (std::size_t index = 0u; index < length; ++index) {
                const auto rank = static_cast<std::uint8_t>((index * 7u + traits::alphabet_size - 1u) % traits::alphabet_size);
                query[query_start + index] = target[target_start + index] = rank;
            }
            if (pattern == 1u) target[target_start + length - 1u] =
                static_cast<std::uint8_t>((target[target_start + length - 1u] + 1u) % traits::alphabet_size);
            if (pattern == 2u) {
                std::fill(query.begin() + query_start, query.begin() + query_start + length, 1u);
                std::fill(target.begin() + target_start, target.begin() + target_start + length, 1u);
                query[query_start] = 0u;
            }
            fixture q{query, query_leading}, t{target, target_leading};
            const auto q_descriptor = q.source.view().template borrow_rank_source<false>(true);
            const auto t_descriptor = t.source.view().template borrow_rank_source<false>(true);
            REQUIRE(q_descriptor.resident.has_value());
            REQUIRE(t_descriptor.resident.has_value());
            REQUIRE(q_descriptor.resident->storage_unit_width == sizeof(Word));
            REQUIRE(t_descriptor.resident->storage_unit_width == sizeof(Word));
            REQUIRE(q_descriptor.resident->leading_symbol_offset == query_leading);
            REQUIRE(t_descriptor.resident->leading_symbol_offset == target_leading);
            compare(q.source, t.source, q.ranks, job_for(length, pattern == 2u
                ? static_cast<std::uint32_t>(length - 1u) : 2u), true);
        }
        if constexpr (seqan3::nucleotide_alphabet<Alphabet>) {
            std::vector<std::uint8_t> query(length + query_start + 1u);
            for (std::size_t index = 0u; index < query.size(); ++index)
                query[index] = static_cast<std::uint8_t>(index % traits::alphabet_size);
            std::vector<std::uint8_t> oriented(query.size());
            for (std::size_t index = 0u; index < query.size(); ++index)
                oriented[index] = seqan3::to_rank(seqan3::complement(
                    seqan3::assign_rank_to(query[query.size() - 1u - index], Alphabet{})));
            std::vector<std::uint8_t> target(length + target_start + 1u);
            std::copy_n(oriented.begin() + query_start, length, target.begin() + target_start);
            fixture q{query, query_leading}, t{target, target_leading};
            compare(q.source, t.source, oriented, job_for(length, 2u, true),
                std::same_as<Alphabet, seq::dna4>, true);
        }
    }
    if constexpr (traits::alphabet_size <= traits::char_mask) {
        // This invalid rank is in acquired lookahead, past the first mismatching rank.
        std::vector<std::uint8_t> q1(65u + query_start + 1u, 0u);
        std::vector<std::uint8_t> t1(65u + target_start + 1u, 1u);
        fixture query1{q1, query_leading}, target1{t1, target_leading};
        target1.corrupt(target_start + 1u);
        invalid_acquisition(query1.source, target1.source, 65u, true, target_start + 1u);
        // An early matching candidate must not hide corruption at the last acquired candidate.
        std::vector<std::uint8_t> q2(513u + query_start + 1u, 1u);
        std::vector<std::uint8_t> t2(513u + target_start + 1u, 1u);
        q2[query_start] = 0u;
        fixture query2{q2, query_leading}, target2{t2, target_leading};
        query2.corrupt(query_start + prefilter_rank_window::chunk_size - 1u);
        invalid_acquisition(query2.source, target2.source, 513u, false,
            query_start + prefilter_rank_window::chunk_size - 1u);
    }
}

/** A single mixed-width pair per alphabet proves the intended checked byte fallback. */
template<sequence_alphabet Alphabet, class Narrow, class Wide>
void fallback_profile() {
    std::vector<std::uint8_t> query(513u + query_start + 1u, 1u);
    std::vector<std::uint8_t> target(513u + target_start + 1u, 1u);
    query[query_start + 64u] = 0u;
    native_fixture<Alphabet, Narrow> q{query, 0u};
    native_fixture<Alphabet, Wide> t{target, 0u};
    REQUIRE(q.source.view().template borrow_rank_source<false>(true).resident.has_value());
    REQUIRE(t.source.view().template borrow_rank_source<false>(true).resident.has_value());
    compare(q.source, t.source, q.ranks, job_for(513u, 2u), false);
}

/** Verify native validated-cache refills and explicit byte materialization without undefined scratch reads. */
void cache_and_provider_controls() {
    using Alphabet = seq::aa27;
    using Word = typename all_storage_unit_byte_widths::template storage_unit<sizeof(std::uint64_t)>;
    std::vector<std::uint8_t> query(513u + query_start + 1u, 1u);
    std::vector<std::uint8_t> target(513u + target_start + 1u, 1u);
    native_fixture<Alphabet, Word> q{query, 1u}, t{target, 2u};
    const std::array jobs{job_for(513u, 2u)};
    const std::array queries{q.source.view().template borrow_rank_source<false>(true)};
    const std::array targets{t.source.view().template borrow_rank_source<false>(true)};
    const auto cached = evaluate_prefilter_job({queries, targets, jobs}, 0u,
        [&q, &t](const prefilter_rank_window& qw, const prefilter_rank_window& tw, const prefilter_job&) {
            REQUIRE(qw.packed != nullptr);
            REQUIRE(tw.packed != nullptr);
            REQUIRE(qw.acquire(0u, 17u));
            REQUIRE(qw.acquire(0u, prefilter_rank_window::chunk_size));
            REQUIRE(qw.contains(0u, prefilter_rank_window::chunk_size));
            REQUIRE_FALSE(qw.materialized);
            REQUIRE(q.source.reads == 0u);
            const auto bytes = qw.window(7u, 1u);
            REQUIRE(bytes.size() == 1u);
            REQUIRE(bytes[0] == 1u);
            REQUIRE(qw.materialized);
            REQUIRE(q.source.reads == 1u);
            REQUIRE(qw.acquire(prefilter_rank_window::chunk_size, 1u));
            REQUIRE(qw.contains(prefilter_rank_window::chunk_size, 1u));
            REQUIRE_FALSE(qw.materialized);
            REQUIRE(q.source.reads == 1u);
            REQUIRE(t.source.reads == 0u);
            return true;
        }, sneaky_snake_detail::scalar_traversal::read_block_size(),
        &resolve_prefilter_packed_operations<sneaky_snake_detail::scalar_traversal>);
    REQUIRE(cached.has_value());
    REQUIRE(*cached);
    // Mandatory failure declines residency; target failure is observed before any query callback.
    std::vector<bool> order;
    q.source.read_order = t.source.read_order = &order;
    q.source.target_side = false;
    t.source.target_side = true;
    q.source.fail_reads = t.source.fail_reads = true;
    for (const bool simd : {false, true}) {
        const std::array failed_queries{q.source.view().template borrow_rank_source<false>(true)};
        const std::array failed_targets{t.source.view().template borrow_rank_source<false>(true)};
        REQUIRE_FALSE(failed_queries[0].resident.has_value());
        REQUIRE_FALSE(failed_targets[0].resident.has_value());
        q.source.reads = t.source.reads = 0u;
        order.clear();
        const auto actual = run_backend(simd, {failed_queries, failed_targets, jobs}, {});
        REQUIRE_FALSE(actual.has_value());
        REQUIRE(actual.error().code == prefilter_input_failure_code::source_read_failed);
        REQUIRE(actual.error().target_source);
        REQUIRE(actual.error().position == target_start);
        REQUIRE(q.source.reads == 0u);
        REQUIRE(t.source.reads == 1u);
        REQUIRE(order == std::vector<bool>{true});
    }
    // Length-only decisions do not acquire even a deliberately failing provider.
    for (const bool unequal : {false, true}) {
        auto length_job = job_for(unequal ? 3u : 0u, 2u);
        if (unequal) length_job.target_length = 4u;
        const std::array length_jobs{length_job};
        const std::array failed_queries{q.source.view().template borrow_rank_source<false>(true)};
        const std::array failed_targets{t.source.view().template borrow_rank_source<false>(true)};
        const auto oracle = sneaky_snake_scalar(
            std::span<const std::uint8_t>{q.ranks}.subspan(query_start, length_job.query_length),
            std::span<const std::uint8_t>{t.ranks}.subspan(target_start, length_job.target_length), 2u);
        for (const bool simd : {false, true}) {
            q.source.reads = t.source.reads = 0u;
            order.clear();
            const auto actual = run_backend(simd, {failed_queries, failed_targets, length_jobs}, {});
            REQUIRE(actual.has_value());
            REQUIRE(actual->size() == 1u);
            REQUIRE(same_result(actual->front(), oracle));
            REQUIRE(q.source.reads == 0u);
            REQUIRE(t.source.reads == 0u);
            REQUIRE(order.empty());
        }
    }
    // The fallback must pin independent cache bytes before another callback overwrites a shared slot.
    std::vector<std::uint8_t> alias_query(513u + query_start + 1u, 0u);
    std::vector<std::uint8_t> alias_target(513u + target_start + 1u, 0u);
    for (std::size_t index = 0u; index < 513u; ++index) {
        const auto rank = static_cast<std::uint8_t>((index + 7u) % seqan3::alphabet_size<Alphabet>);
        alias_query[query_start + index] = alias_target[target_start + index] = rank;
    }
    alias_target[target_start + 64u] = static_cast<std::uint8_t>(
        (alias_target[target_start + 64u] + 1u) % seqan3::alphabet_size<Alphabet>);
    native_fixture<Alphabet, Word> alias_q{alias_query, 1u}, alias_t{alias_target, 2u};
    std::array<std::uint8_t, prefilter_rank_window::chunk_size> shared_slot{};
    alias_q.source.shared_slot = alias_t.source.shared_slot = &shared_slot;
    REQUIRE_FALSE(alias_q.source.view().template borrow_rank_source<false>(true).resident.has_value());
    REQUIRE_FALSE(alias_t.source.view().template borrow_rank_source<false>(true).resident.has_value());
    compare(alias_q.source, alias_t.source, alias_q.ranks, jobs[0], false);
}

/** Expand only the registered storage layouts, keeping run dimensions outside templates. */
template<sequence_alphabet Alphabet>
void alphabet_profiles(std::size_t& profiles) {
    [&profiles]<std::size_t... Widths>(supported_storage_unit_byte_widths<Widths...>) {
        ((profile<Alphabet, typename all_storage_unit_byte_widths::template storage_unit<Widths>>(), ++profiles), ...);
        constexpr std::array widths{Widths...};
        using Narrow = typename all_storage_unit_byte_widths::template storage_unit<widths.front()>;
        using Wide = typename all_storage_unit_byte_widths::template storage_unit<widths.back()>;
        static_assert(!std::same_as<Narrow, Wide>);
        fallback_profile<Alphabet, Narrow, Wide>();
    }(all_storage_unit_byte_widths{});
}

/** One existing case covers all registered layouts without a second target or test case. */
void run() {
    std::size_t profiles = 0u;
    std::apply([&profiles](auto... entry) {
        (alphabet_profiles<typename decltype(entry)::alphabet_type>(profiles), ...);
    }, supported_biological_alphabets::alphabet_entries_tuple{});
    REQUIRE(profiles == std::tuple_size_v<supported_biological_alphabets::alphabet_entries_tuple>
        * all_storage_unit_byte_widths::width_count);
    cache_and_provider_controls();
}
} // namespace manual_native_sneaky_probe
// END_TEMPORARY_NATIVE_SNEAKY_PROBE -- DO NOT COMMIT

TEST_CASE(
    "SneakySnake Highway batch exactly matches scalar for arbitrary ranks",
    "[component:bioseq][layer:unit][feature:prefilter][resource:highway]")
{
    // TEMPORARY native-path validation; remove with the marked helper.
    manual_native_sneaky_probe::run();
    const hn::ScalableTag<std::uint32_t> d;
    const std::size_t job_count = std::max<std::size_t>(
        97u,
        hn::Lanes(d) * 3u + 5u);
    batch_fixture fixture = random_batch(job_count);
    const rank_batch input{
        .query_ranks = fixture.query_ranks,
        .target_ranks = fixture.target_ranks,
        .jobs = fixture.jobs,
    };

    const std::array<sneaky_snake_options, 4u> option_sets{
        sneaky_snake_options{},
        sneaky_snake_options{
            .submaze_width = 7u,
            .max_escape_segments = 7u,
        },
        sneaky_snake_options{
            .submaze_width = 13u,
            .max_escape_segments = 2u,
        },
        sneaky_snake_options{
            .submaze_width = 0u,
            .max_escape_segments = 0u,
        },
    };

    for (const sneaky_snake_options options : option_sets) {
        const auto scalar = checked_value(sneaky_snake_scalar_batch(input, options));
        const auto simd = checked_value(fixed_sneaky_snake_kernel()(input, options));
        REQUIRE(scalar.size() == fixture.jobs.size());
        REQUIRE(simd.size() == scalar.size());

        for (std::size_t index = 0u; index < scalar.size(); ++index) {
            const std::uint32_t distance = exact_levenshtein(
                fixture.queries[index], fixture.targets[index]);
            CAPTURE(
                index,
                options.submaze_width,
                options.max_escape_segments,
                distance,
                scalar[index].lower_bound,
                simd[index].lower_bound);
            REQUIRE(same_result(simd[index], scalar[index]));
            REQUIRE(scalar[index].lower_bound <= distance);
            if (distance <= fixture.jobs[index].max_edits) {
                REQUIRE(is_prefilter_accepted(scalar[index]));
            }
        }
    }
}

TEST_CASE(
    "SneakySnake Highway scans full vectors and exact mismatch boundaries",
    "[component:bioseq][layer:unit][feature:prefilter][resource:highway]")
{
    const hn::ScalableTag<std::uint8_t> byte_d;
    const std::uint32_t lanes = static_cast<std::uint32_t>(
        hn::Lanes(byte_d));
    const std::uint32_t length = 3u * lanes + 5u;
    std::vector<std::uint32_t> mismatch_positions{
        1u,
        lanes - 1u,
        lanes,
        lanes + 1u,
        2u * lanes - 1u,
        2u * lanes,
        2u * lanes + 1u,
        length - 1u,
        length,
    };
    std::sort(mismatch_positions.begin(), mismatch_positions.end());
    mismatch_positions.erase(
        std::unique(mismatch_positions.begin(), mismatch_positions.end()),
        mismatch_positions.end());

    std::vector<std::uint8_t> query_ranks;
    std::vector<std::uint8_t> target_ranks;
    std::vector<prefilter_job> jobs;
    for (const std::uint32_t mismatch : mismatch_positions) {
        query_ranks.push_back(17u);
        target_ranks.push_back(19u);
        const std::uint32_t query_offset = static_cast<std::uint32_t>(
            query_ranks.size());
        const std::uint32_t target_offset = static_cast<std::uint32_t>(
            target_ranks.size());
        query_ranks.insert(query_ranks.end(), length, 26u);
        target_ranks.insert(target_ranks.end(), length, 26u);
        if (mismatch < length) {
            target_ranks[static_cast<std::size_t>(target_offset) + mismatch]
                = 0u;
        }
        query_ranks.push_back(23u);
        target_ranks.push_back(29u);
        jobs.push_back(prefilter_job{
            .query_start = query_offset,
            .target_start = target_offset,
            .query_length = length,
            .target_length = length,
            .max_edits = 0u,
        });
    }

    const rank_batch input{
        .query_ranks = query_ranks,
        .target_ranks = target_ranks,
        .jobs = jobs,
    };
    const auto scalar = checked_value(sneaky_snake_scalar_batch(input));
    const auto simd = checked_value(fixed_sneaky_snake_kernel()(input, {}));
    REQUIRE(simd.size() == scalar.size());
    for (std::size_t index = 0u; index < scalar.size(); ++index) {
        CAPTURE(index, mismatch_positions[index], lanes, length);
        REQUIRE(same_result(simd[index], scalar[index]));
        if (mismatch_positions[index] < length) {
            REQUIRE(scalar[index].lower_bound == 1u);
            REQUIRE_FALSE(is_prefilter_accepted(scalar[index]));
        } else {
            REQUIRE(scalar[index].lower_bound == 0u);
            REQUIRE(is_prefilter_accepted(scalar[index]));
        }
    }
}

TEST_CASE(
    "SneakySnake generated fixed-target candidate mask preserves the longest diagonal",
    "[component:bioseq][layer:unit][feature:prefilter][resource:highway]")
{
    constexpr std::uint32_t submaze_width = 512u;
    constexpr std::uint32_t length = 2u * submaze_width;
    constexpr std::uint32_t position = submaze_width;
    constexpr std::uint32_t candidate_block = 64u;
    constexpr std::uint32_t max_edits = 4u * candidate_block + 3u;
    constexpr std::uint32_t lower = position - max_edits;
    constexpr std::uint32_t winning_start = position - candidate_block;

    std::vector<std::uint8_t> target(length, 1u);
    target[lower] = 0u;
    target[lower + candidate_block - 1u] = 0u;
    target[lower + candidate_block] = 0u;
    target[winning_start] = 0u;
    target[winning_start + 1u] = 2u;

    // Keep the first chunk equal. In the second  chunk, place the longest
    // matching diagonal past three 64-position candidate blocks to check that
    // the SIMD search reaches it.
    std::copy_n(
        target.begin() + winning_start,
        candidate_block,
        target.begin() + position);
    for (std::uint32_t index = position + candidate_block;
         index < length;
         ++index) {
        target[index] = static_cast<std::uint8_t>(
            3u + index % (seqan3::alphabet_size<seq::aa27> - 3u));
    }

    std::vector<std::uint8_t> query = target;
    std::copy_n(
        target.begin() + position,
        submaze_width,
        query.begin() + winning_start);

    const std::array<prefilter_job, 1u> jobs{prefilter_job{
        .query_start = 0u,
        .target_start = 0u,
        .query_length = length,
        .target_length = length,
        .max_edits = max_edits,
    }};
    const rank_batch input{
        .query_ranks = query,
        .target_ranks = target,
        .jobs = jobs,
    };
    const sneaky_snake_options options{
        .submaze_width = submaze_width,
        .max_escape_segments = submaze_width,
    };

    const auto scalar = checked_value(sneaky_snake_scalar_batch(input, options));
    const auto fixed = checked_value(fixed_sneaky_snake_kernel()(input, options));
    REQUIRE(scalar.size() == 1u);
    REQUIRE(fixed.size() == scalar.size());
    REQUIRE(scalar.front().lower_bound == 0u);
    REQUIRE(is_prefilter_accepted(scalar.front()));
    REQUIRE(same_result(fixed.front(), scalar.front()));
}

TEST_CASE(
    "SneakySnake hardware Vulkan batch matches scalar across submaze boundaries",
    "[component:bioseq][layer:unit][feature:prefilter][resource:gpu]")
{
    namespace vp = bioseq::prefilter;
    namespace gv = bioseq::gpu::vulkan;
    if (!vp::is_vulkan_sneaky_snake_supported()) {
        SKIP("hardware Vulkan device unavailable");
    }

    const auto require_gpu_parity = [](const batch_fixture& fixture) {
        const rank_batch input{
            .query_ranks = fixture.query_ranks,
            .target_ranks = fixture.target_ranks,
            .jobs = fixture.jobs,
        };
        const auto scalar = checked_value(sneaky_snake_scalar_batch(input));
        const auto packed_query = packed_ranks(fixture.query_ranks);
        const auto packed_target = packed_ranks(fixture.target_ranks);
        const auto gpu = vp::run_sneaky_snake_vulkan_batch(
            bioseq::storage::make_lazy_sequence_view(packed_query),
            bioseq::storage::make_lazy_sequence_view(packed_target), fixture.jobs, ::bioseq::simd_dispatch::highway_test_kernel_table().common.validate_packed_sequence_ranks);

        // Reuse the command shape with new target bytes. Supported equal-length
        // pairs become exact matches, so changed results confirm that replay reads
        // the latest host writes.
        std::vector<std::uint8_t> replay_targets = fixture.target_ranks;
        for (const prefilter_job& job : fixture.jobs) {
            if (job.query_length == job.target_length
                && job.query_length != 0u
                && job.max_edits <= gv::k_gpu_sneaky_snake_max_edits) {
                std::copy_n(
                    fixture.query_ranks.begin() + job.query_start,
                    job.query_length,
                    replay_targets.begin() + job.target_start);
            }
        }
        const rank_batch replay_input{
            .query_ranks = fixture.query_ranks,
            .target_ranks = replay_targets,
            .jobs = fixture.jobs,
        };
        const auto replay_scalar = checked_value(sneaky_snake_scalar_batch(replay_input));
        const auto packed_replay = packed_ranks(replay_targets);
        const auto replayed = vp::run_sneaky_snake_vulkan_batch(
            bioseq::storage::make_lazy_sequence_view(packed_query),
            bioseq::storage::make_lazy_sequence_view(packed_replay), fixture.jobs, ::bioseq::simd_dispatch::highway_test_kernel_table().common.validate_packed_sequence_ranks);
        REQUIRE(gpu.size() == scalar.size());
        REQUIRE(replayed.size() == gpu.size());
        bool observed_changed_result = false;
        for (std::size_t index = 0u; index < scalar.size(); ++index) {
            CAPTURE(index, fixture.jobs[index].query_length,
                    fixture.jobs[index].target_length,
                    fixture.jobs[index].max_edits);
            if (fixture.jobs[index].max_edits
                > gv::k_gpu_sneaky_snake_max_edits) {
                CHECK(gpu[index].lower_bound == 0u);
                CHECK(gpu[index].status
                    == static_cast<std::uint32_t>(
                        prefilter_status::unsupported_conservative));
                CHECK(gpu[index].is_accepted == 1u);
                CHECK(gpu[index].diagnostics == 0u);
                CHECK(replayed[index].lower_bound == 0u);
                CHECK(replayed[index].status
                    == static_cast<std::uint32_t>(
                        prefilter_status::unsupported_conservative));
                CHECK(replayed[index].is_accepted == 1u);
                CHECK(replayed[index].diagnostics == 0u);
                continue;
            }
            CHECK(gpu[index].lower_bound == scalar[index].lower_bound);
            CHECK(gpu[index].status == scalar[index].status);
            CHECK(gpu[index].is_accepted == scalar[index].is_accepted);
            CHECK(gpu[index].diagnostics == scalar[index].diagnostics);
            CHECK(replayed[index].lower_bound
                == replay_scalar[index].lower_bound);
            CHECK(replayed[index].status == replay_scalar[index].status);
            CHECK(replayed[index].is_accepted == replay_scalar[index].is_accepted);
            CHECK(replayed[index].diagnostics == replay_scalar[index].diagnostics);
            observed_changed_result |=
                replay_scalar[index].lower_bound != scalar[index].lower_bound
                || replay_scalar[index].status != scalar[index].status
                || replay_scalar[index].is_accepted != scalar[index].is_accepted;
        }
        CHECK(observed_changed_result);
    };

    // Small edit limits select one GPU invocation per job. Check slice
    // offsets and chunk boundaries with a partially filled workgroup.
    batch_fixture low_edit_fixture = random_batch(12u);
    for (prefilter_job& job : low_edit_fixture.jobs) {
        job.max_edits = std::min(job.max_edits, 8u);
    }
    require_gpu_parity(low_edit_fixture);

    batch_fixture high_edit_fixture;
    const auto append_job = [&high_edit_fixture](
        std::span<const std::uint8_t> query,
        std::span<const std::uint8_t> target,
        std::uint32_t max_edits) {
        high_edit_fixture.query_ranks.push_back(255u);
        const std::uint32_t query_offset = static_cast<std::uint32_t>(
            high_edit_fixture.query_ranks.size());
        high_edit_fixture.query_ranks.insert(
            high_edit_fixture.query_ranks.end(), query.begin(), query.end());
        high_edit_fixture.query_ranks.push_back(254u);

        high_edit_fixture.target_ranks.push_back(253u);
        const std::uint32_t target_offset = static_cast<std::uint32_t>(
            high_edit_fixture.target_ranks.size());
        high_edit_fixture.target_ranks.insert(
            high_edit_fixture.target_ranks.end(), target.begin(), target.end());
        high_edit_fixture.target_ranks.push_back(252u);

        high_edit_fixture.jobs.push_back(prefilter_job{
            .query_start = query_offset,
            .target_start = target_offset,
            .query_length = static_cast<std::uint32_t>(query.size()),
            .target_length = static_cast<std::uint32_t>(target.size()),
            .max_edits = max_edits,
        });
    };

    std::vector<std::uint8_t> shifted_query(600u);
    std::vector<std::uint8_t> shifted_target(600u, 26u);
    for (std::size_t index = 0u; index < shifted_query.size(); ++index) {
        shifted_query[index] = static_cast<std::uint8_t>(
            (index * 37u + index / 7u * 11u) % seqan3::alphabet_size<seq::aa27>);
    }
    constexpr std::size_t k_winning_shift = 17u;
    for (std::size_t index = k_winning_shift;
         index < shifted_target.size(); ++index) {
        shifted_target[index] = shifted_query[index - k_winning_shift];
    }
    append_job(shifted_query, shifted_target, 31u);
    append_job(shifted_query, shifted_target, 32u);
    append_job({}, {}, 31u);
    const std::array<std::uint8_t, 1u> one_rank{7u};
    append_job({}, one_rank, 31u);
    require_gpu_parity(high_edit_fixture);
}

TEST_CASE(
    "SneakySnake malformed batch slices return typed failures without reading buffers",
    "[component:bioseq][layer:unit][feature:prefilter]")
{
    const std::array<std::uint8_t, 2u> query{1u, 2u};
    const std::array<std::uint8_t, 2u> target{1u, 2u};
    const std::array<prefilter_job, 1u> jobs{prefilter_job{
        .query_start = 2u,
        .target_start = 0u,
        .query_length = 1u,
        .target_length = 1u,
        .max_edits = 0u,
    }};
    const rank_batch input{
        .query_ranks = query,
        .target_ranks = target,
        .jobs = jobs,
    };

    REQUIRE_FALSE(prefilter_job_in_bounds(input, jobs.front()));
    const auto scalar = sneaky_snake_scalar_batch(input);
    const auto simd = fixed_sneaky_snake_kernel()(input, {});
    REQUIRE_FALSE(scalar.has_value());
    REQUIRE_FALSE(simd.has_value());
    REQUIRE(scalar.error().code == prefilter_input_failure_code::invalid_descriptor);
    REQUIRE(simd.error().code == scalar.error().code);

}

} // namespace
} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
