// Differential tests for the PG-free hash32 direct inverted index.

#include <tests/support/common.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

import bioseq.indexing.compact_hash32_direct_index;

namespace {

namespace idx = bioseq::indexing;

[[nodiscard]] constexpr idx::direct_index_identity identity(
        std::uint64_t target_length) noexcept {
    return idx::direct_index_identity{
        .generation_id = 0x1112131415161718ULL,
        .source_identity = 0x2122232425262728ULL,
        .storage_contract_checksum = 0x3132333435363738ULL,
        .target_length = target_length,
    };
}

[[nodiscard]] idx::direct_index_build_options options(
        std::uint64_t target_length,
        std::uint64_t fragment_span = 0u,
        std::uint64_t bucket_override = 0u) noexcept {
    return idx::direct_index_build_options{
        .identity = identity(target_length),
        .fragment_span_bp = fragment_span,
        .bucket_count_override = bucket_override,
    };
}

template <class p_unsigned_integer>
requires std::is_unsigned_v<p_unsigned_integer>
[[nodiscard]] constexpr p_unsigned_integer reference_read_le(
        std::span<const std::byte> input,
        std::size_t offset) noexcept {
    p_unsigned_integer value = 0u;
    for (std::size_t byte = 0u; byte < sizeof(p_unsigned_integer); ++byte) {
        value |= static_cast<p_unsigned_integer>(
            std::to_integer<std::uint8_t>(input[offset + byte]))
            << (byte * 8u);
    }
    return value;
}

template <class p_unsigned_integer>
requires std::is_unsigned_v<p_unsigned_integer>
constexpr void reference_write_le(
        std::span<std::byte> output,
        std::size_t offset,
        p_unsigned_integer value) noexcept {
    for (std::size_t byte = 0u; byte < sizeof(p_unsigned_integer); ++byte) {
        output[offset + byte] = static_cast<std::byte>(value & p_unsigned_integer{0xffu});
        value >>= 8u;
    }
}

/// Independent bit-at-a-time CRC-64/ECMA-182 oracle.  The implementation
/// deliberately does not call the production lookup-table codec.
[[nodiscard]] constexpr std::uint64_t reference_crc64(
        std::span<const std::byte> bytes) noexcept {
    constexpr std::uint64_t polynomial = 0x42F0E1EBA9EA3693ULL;
    std::uint64_t checksum = 0u;
    for (const std::byte byte : bytes) {
        checksum ^= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(byte)) << 56u;
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            checksum = (checksum & (std::uint64_t{1u} << 63u)) != 0u
                ? (checksum << 1u) ^ polynomial
                : checksum << 1u;
        }
    }
    return checksum;
}

void reference_reseal_header(std::vector<std::byte>& image) {
    std::vector<std::byte> header(
        image.begin(),
        image.begin() + idx::k_direct_inverted_index_header_bytes);
    std::fill_n(
        header.begin()
            + static_cast<std::ptrdiff_t>(
                idx::k_direct_inverted_index_header_crc_offset),
        idx::k_direct_inverted_index_checksum_bytes,
        std::byte{0});
    reference_write_le<std::uint64_t>(
        image,
        idx::k_direct_inverted_index_header_crc_offset,
        reference_crc64(header));
}

void reference_reseal_compact_hash32_header(
        std::vector<std::byte>& image) {
    REQUIRE(image.size() >= idx::k_compact_hash32_direct_index_header_bytes);
    std::array<std::byte, idx::k_compact_hash32_direct_index_header_bytes> header{};
    std::copy_n(image.begin(), header.size(), header.begin());
    std::fill_n(
        header.begin()
            + static_cast<std::ptrdiff_t>(idx::k_compact_hash32_header_crc_offset),
        idx::k_direct_inverted_index_checksum_bytes,
        std::byte{0});
    reference_write_le<std::uint64_t>(
        image,
        idx::k_compact_hash32_header_crc_offset,
        reference_crc64(header));
}

void reference_reseal_controls(std::vector<std::byte>& image) {
    const std::uint64_t offset = reference_read_le<std::uint64_t>(image, 112u);
    const std::uint64_t bytes = reference_read_le<std::uint64_t>(image, 120u);
    reference_write_le<std::uint64_t>(
        image,
        idx::k_direct_inverted_index_controls_crc_offset,
        reference_crc64(std::span<const std::byte>{image}.subspan(
            static_cast<std::size_t>(offset),
            static_cast<std::size_t>(bytes))));
    reference_reseal_header(image);
}

using oracle_type = std::map<std::uint32_t, std::vector<idx::direct_index_occurrence>>;

[[nodiscard]] oracle_type make_oracle(
        std::span<const idx::direct_index_occurrence> input) {
    oracle_type oracle;
    for (const auto& occurrence : input) {
        oracle[occurrence.hash].push_back(occurrence);
    }
    return oracle;
}

[[nodiscard]] std::uint64_t oracle_strand_zero_count(
        const std::vector<idx::direct_index_occurrence>& occurrences) noexcept {
    return static_cast<std::uint64_t>(std::count_if(
        occurrences.begin(), occurrences.end(),
        [](const auto& occurrence) {
            return !occurrence.target_strand_one;
        }));
}

struct collected_postings {
    std::vector<std::vector<std::uint64_t>> by_selection{};
    std::uint64_t callback_count = 0u;
};

bool collect_posting(void* opaque, const idx::direct_index_posting& posting) {
    auto& collected = *static_cast<collected_postings*>(opaque);
    if (posting.selection_ordinal >= collected.by_selection.size()) {
        return false;
    }
    collected.by_selection[posting.selection_ordinal].push_back(
        posting.encoded_position);
    ++collected.callback_count;
    return posting.target_strand_one
        == ((posting.encoded_position & 1u) != 0u);
}

[[nodiscard]] std::vector<std::uint64_t> expected_postings(
        std::vector<idx::direct_index_occurrence> occurrences,
        std::uint64_t fragment_span) {
    std::sort(
        occurrences.begin(), occurrences.end(),
        [fragment_span](const auto& left, const auto& right) {
            const std::uint64_t left_fragment = fragment_span == 0u
                ? 0u : left.target_bp / fragment_span;
            const std::uint64_t right_fragment = fragment_span == 0u
                ? 0u : right.target_bp / fragment_span;
            return std::tuple{
                    left_fragment, left.target_strand_one, left.target_bp}
                < std::tuple{
                    right_fragment, right.target_strand_one, right.target_bp};
        });
    std::vector<std::uint64_t> result;
    result.reserve(occurrences.size());
    for (const auto& occurrence : occurrences) {
        result.push_back(
            (occurrence.target_bp << 1u)
            | static_cast<std::uint64_t>(occurrence.target_strand_one));
    }
    return result;
}

[[nodiscard]] std::vector<std::byte> build_or_fail(
        std::span<const idx::direct_index_occurrence> input,
        const idx::direct_index_build_options& build_options) {
    auto image = idx::build_direct_inverted_index(input, build_options);
    REQUIRE(image.has_value());
    return std::move(*image);
}

[[nodiscard]] idx::direct_inverted_index_view validate_or_fail(
        const std::vector<std::byte>& image,
        const idx::direct_index_identity& expected_identity) {
    auto view = idx::validate_direct_inverted_index(image, expected_identity);
    REQUIRE(view.has_value());
    return *view;
}

void require_matches_oracle(
        const idx::direct_index_query_plan& plan,
        std::span<const idx::direct_index_query_occurrence> queries,
        const oracle_type& oracle,
        std::uint64_t max_occ) {
    REQUIRE(plan.results.size() == queries.size());
    for (std::size_t ordinal = 0u; ordinal < queries.size(); ++ordinal) {
        const auto found = oracle.find(queries[ordinal].hash);
        const auto& result = plan.results[ordinal];
        if (found == oracle.end()) {
            CHECK_FALSE(result.present);
            CHECK_FALSE(result.positions_selected);
            CHECK(result.global_count == 0u);
            CHECK(result.forward_count == 0u);
            CHECK(result.reverse_count == 0u);
            continue;
        }
        const std::uint64_t total = found->second.size();
        const std::uint64_t strand_zero =
            oracle_strand_zero_count(found->second);
        CHECK(result.present);
        CHECK(result.global_count == total);
        CHECK(result.forward_count
            == (queries[ordinal].query_strand_one
                ? total - strand_zero : strand_zero));
        CHECK(result.reverse_count
            == total - result.forward_count);
        CHECK(result.positions_selected == (total <= max_occ));
        CHECK((result.positions_selected
                ? result.selection_ordinal
                    != idx::k_direct_inverted_index_no_selection
                : result.selection_ordinal
                    == idx::k_direct_inverted_index_no_selection));
    }
}

struct compact_test_run {
    idx::compact_hash32_sorted_run_header header{};
    std::vector<std::uint64_t> encoded_positions{};
};

struct compact_test_source {
    std::vector<compact_test_run> runs{};
    std::vector<compact_test_run> replay_runs{};
    bool use_replay_runs_after_first_scan = false;
    std::uint64_t scans = 0u;
};

[[nodiscard]] std::expected<void, idx::direct_inverted_index_error>
scan_compact_test_runs(
        void* opaque,
        const idx::compact_hash32_sorted_run_receiver& receiver) {
    auto& source = *static_cast<compact_test_source*>(opaque);
    ++source.scans;
    const auto& runs = source.use_replay_runs_after_first_scan
            && source.scans > 1u
        ? source.replay_runs : source.runs;
    for (const compact_test_run& run : runs) {
        if (auto begun = receiver.begin_run(run.header); !begun) {
            return std::unexpected(begun.error());
        }
        const std::size_t split = run.encoded_positions.size() / 2u;
        if (auto first = receiver.consume_positions(
                std::span<const std::uint64_t>{run.encoded_positions}.first(
                    split)); !first) {
            return std::unexpected(first.error());
        }
        if (auto second = receiver.consume_positions(
                std::span<const std::uint64_t>{run.encoded_positions}.subspan(
                    split)); !second) {
            return std::unexpected(second.error());
        }
    }
    return {};
}

[[nodiscard]] idx::compact_hash32_sorted_run_source compact_source_ref(
        compact_test_source& source) noexcept {
    return idx::compact_hash32_sorted_run_source{
        .context = &source,
        .scan_sorted_runs_fn = &scan_compact_test_runs,
    };
}

[[nodiscard]] constexpr idx::compact_hash32_geometry compact_geometry(
        std::uint8_t bits) noexcept {
    return idx::compact_hash32_geometry{
        .hash_domain_bits = bits,
        .max_hash = bits == 32u
            ? (std::numeric_limits<std::uint32_t>::max)()
            : static_cast<std::uint32_t>(
                (std::uint64_t{1u} << bits) - 1u),
    };
}

[[nodiscard]] idx::compact_hash32_build_options compact_options(
        std::uint64_t target_length,
        std::uint32_t max_occ_cap,
        std::uint8_t hash_bits = 9u) noexcept {
    return idx::compact_hash32_build_options{
        .identity = identity(target_length),
        .max_occ_cap = max_occ_cap,
        .geometry = compact_geometry(hash_bits),
    };
}

[[nodiscard]] idx::compact_hash32_bounded_open_options compact_bounded_options(
        const idx::compact_hash32_build_options& build_options,
        std::optional<std::uint64_t> expected_direct_header_crc64 =
            std::nullopt) noexcept {
    return idx::compact_hash32_bounded_open_options{
        .expected_identity = build_options.identity,
        .expected_max_occ_cap = build_options.max_occ_cap,
        .expected_geometry = build_options.geometry,
        .expected_direct_header_crc64 = expected_direct_header_crc64,
    };
}

[[nodiscard]] constexpr std::uint64_t encoded_position(
        std::uint64_t bp,
        bool strand_one) noexcept {
    return (bp << 1u) | static_cast<std::uint64_t>(strand_one);
}

[[nodiscard]] std::vector<std::byte> build_compact_or_fail(
        compact_test_source& source,
        const idx::compact_hash32_build_options& build_options) {
    auto image = idx::build_compact_hash32_direct_index(
        compact_source_ref(source), build_options);
    REQUIRE(image.has_value());
    return std::move(*image);
}

[[nodiscard]] idx::compact_hash32_direct_index_view validate_compact_or_fail(
        const std::vector<std::byte>& image,
        const idx::compact_hash32_build_options& build_options) {
    auto view = idx::validate_compact_hash32_direct_index(
        image,
        build_options.identity,
        build_options.max_occ_cap,
        build_options.geometry);
    REQUIRE(view.has_value());
    return *view;
}

void reference_reseal_compact_hash32_payload_and_header(
        std::vector<std::byte>& image) {
    const auto reseal_section = [&image](
            std::size_t crc_offset,
            std::size_t section_offset_field,
            std::size_t section_bytes_field) {
        const std::uint64_t offset =
            reference_read_le<std::uint64_t>(
                image, section_offset_field);
        const std::uint64_t bytes =
            reference_read_le<std::uint64_t>(
                image, section_bytes_field);
        REQUIRE(offset <= image.size());
        REQUIRE(bytes <= image.size() - offset);
        reference_write_le<std::uint64_t>(
            image,
            crc_offset,
            reference_crc64(std::span<const std::byte>{image}.subspan(
                static_cast<std::size_t>(offset),
                static_cast<std::size_t>(bytes))));
    };
    reseal_section(idx::k_compact_hash32_membership_crc_offset, 112u, 120u);
    reseal_section(idx::k_compact_hash32_rank_crc_offset, 128u, 136u);
    reseal_section(idx::k_compact_hash32_descriptors_crc_offset, 144u, 152u);
    reseal_section(idx::k_compact_hash32_postings_crc_offset, 160u, 168u);
    reference_reseal_compact_hash32_header(image);
}

[[nodiscard]] std::expected<idx::compact_hash32_bounded_open_view,
    idx::direct_inverted_index_error>
open_compact_bounded_owned(
        std::vector<std::byte> image,
        const idx::compact_hash32_bounded_open_options& open_options) {
    auto owner = std::make_shared<std::vector<std::byte>>(std::move(image));
    const std::span<const std::byte> direct_section{
        owner->data(), owner->size()};
    std::shared_ptr<const void> mapped_owner = owner;
    return idx::open_compact_hash32_direct_index_bounded(
        std::move(mapped_owner), direct_section, open_options);
}

} // namespace

TEST_CASE(
    "direct hash32 index differentially resolves absent singleton duplicate and "
    "unsorted query occurrences",
    "[component:bioseq][layer:unit][feature:index]") {
    const std::vector<idx::direct_index_occurrence> input{
        {.hash = 0u, .target_bp = 3u, .target_strand_one = false},
        {.hash = (std::numeric_limits<std::uint32_t>::max)(),
            .target_bp = 11u, .target_strand_one = true},
        // An exact duplicate occurrence remains authoritative, not deduped.
        {.hash = (std::numeric_limits<std::uint32_t>::max)(),
            .target_bp = 11u, .target_strand_one = true},
        {.hash = 42u, .target_bp = 90u, .target_strand_one = true},
        {.hash = 42u, .target_bp = 10u, .target_strand_one = false},
        {.hash = 42u, .target_bp = 20u, .target_strand_one = false},
    };
    const auto build_options = options(1'000u, 0u, 8u);
    const auto plan = idx::plan_direct_inverted_index(input, build_options);
    REQUIRE(plan.has_value());
    CHECK(plan->posting_width_bytes == 4u);
    CHECK(plan->unique_hash_count == 3u);
    CHECK(plan->occurrence_count == input.size());
    CHECK(plan->occupancy_bytes == plan->bucket_count / 8u);
    CHECK(plan->rank_bytes
        == ((plan->bucket_count
                + idx::k_direct_inverted_index_rank_group_buckets - 1u)
            / idx::k_direct_inverted_index_rank_group_buckets + 1u) * 8u);
    CHECK(plan->controls_bytes
        == plan->unique_hash_count * idx::k_direct_inverted_index_control_bytes);
    CHECK(plan->summaries_bytes == 0u);
    CHECK(plan->postings_bytes == input.size() * 4u);
    CHECK(plan->image_bytes == plan->postings_offset + plan->postings_bytes);
    CHECK(plan->estimated_build_scratch_bytes != 0u);

    const auto image = build_or_fail(input, build_options);
    const auto view = validate_or_fail(image, build_options.identity);
    const std::vector<idx::direct_index_query_occurrence> queries{
        {.hash = 42u, .query_strand_one = true},
        {.hash = 999u, .query_strand_one = false},
        {.hash = 0u, .query_strand_one = false},
        {.hash = (std::numeric_limits<std::uint32_t>::max)(),
            .query_strand_one = false},
        {.hash = 42u, .query_strand_one = false},
        {.hash = (std::numeric_limits<std::uint32_t>::max)(),
            .query_strand_one = true},
    };
    const auto query_plan = idx::query_direct_inverted_index(view, queries);
    REQUIRE(query_plan.has_value());
    const oracle_type oracle = make_oracle(input);
    require_matches_oracle(
        *query_plan, queries, oracle,
        (std::numeric_limits<std::uint64_t>::max)());
    // Duplicate query occurrences share one unique target selection.
    CHECK(query_plan->selections.size() == oracle.size());
    CHECK(query_plan->stats.unique_selected_hash_count == oracle.size());
    CHECK(query_plan->stats.selected_posting_count == input.size());

    collected_postings collected{
        .by_selection = std::vector<std::vector<std::uint64_t>>(
            query_plan->selections.size()),
    };
    const auto visited = idx::visit_direct_inverted_index_postings(
        view, *query_plan, &collected, &collect_posting);
    REQUIRE(visited.has_value());
    CHECK(visited->postings_visited == input.size());
    CHECK(collected.callback_count == input.size());
    for (std::size_t selection = 0u;
         selection < query_plan->selections.size(); ++selection) {
        const auto hash = query_plan->selections[selection].hash;
        REQUIRE(oracle.contains(hash));
        CHECK(collected.by_selection[selection]
            == expected_postings(oracle.at(hash), 0u));
    }
}

TEST_CASE(
    "direct hash32 summaries give exact both-strand layouts and visit heavy "
    "selected postings once",
    "[component:bioseq][layer:unit][feature:index]") {
    std::vector<idx::direct_index_occurrence> input;
    constexpr std::uint64_t heavy_count = 1'001u;
    input.reserve(heavy_count + 4u);
    for (std::uint64_t ordinal = 0u; ordinal < heavy_count; ++ordinal) {
        input.push_back(idx::direct_index_occurrence{
            .hash = 7u,
            .target_bp = (ordinal * 37u) % 9'900u,
            .target_strand_one = ordinal % 3u == 0u,
        });
    }
    for (std::uint64_t ordinal = 0u; ordinal < 4u; ++ordinal) {
        input.push_back(idx::direct_index_occurrence{
            .hash = 9u,
            .target_bp = 50u + ordinal * 100u,
            .target_strand_one = ordinal >= 2u,
        });
    }
    const auto build_options = options(10'000u, 100u, 8u);
    const auto image = build_or_fail(input, build_options);
    const auto view = validate_or_fail(image, build_options.identity);
    REQUIRE(view.has_run_summaries());
    CHECK(view.header.run_summary_count > view.header.unique_hash_count);
    const oracle_type oracle = make_oracle(input);

    const std::vector<idx::direct_index_query_occurrence> heavy_queries{
        {.hash = 7u, .query_strand_one = false},
        {.hash = 7u, .query_strand_one = true},
    };
    const auto accepted = idx::query_direct_inverted_index(
        view, heavy_queries, heavy_count);
    REQUIRE(accepted.has_value());
    require_matches_oracle(*accepted, heavy_queries, oracle, heavy_count);
    REQUIRE(accepted->selections.size() == 1u);
    CHECK(accepted->stats.selected_posting_count == heavy_count);
    std::set<std::uint64_t> heavy_fragments;
    for (const auto& occurrence : oracle.at(7u)) {
        heavy_fragments.insert(occurrence.target_bp / 100u);
    }
    CHECK(accepted->stats.run_summaries_examined
        == heavy_fragments.size());

    collected_postings collected{
        .by_selection = std::vector<std::vector<std::uint64_t>>(1u),
    };
    const auto visited = idx::visit_direct_inverted_index_postings(
        view, *accepted, &collected, &collect_posting);
    REQUIRE(visited.has_value());
    // Two query orientations still read the selected target postings once.
    CHECK(visited->postings_visited == heavy_count);
    CHECK(collected.callback_count == heavy_count);
    CHECK(collected.by_selection.front()
        == expected_postings(oracle.at(7u), 100u));

    const auto rejected = idx::query_direct_inverted_index(
        view, heavy_queries, heavy_count - 1u);
    REQUIRE(rejected.has_value());
    require_matches_oracle(
        *rejected, heavy_queries, oracle, heavy_count - 1u);
    CHECK(rejected->selections.empty());
    CHECK(rejected->spans.empty());
    CHECK(rejected->stats.selected_posting_count == 0u);

    const std::array boundary_query{
        idx::direct_index_query_occurrence{
            .hash = 9u, .query_strand_one = false},
    };
    const auto boundary_accepted = idx::query_direct_inverted_index(
        view, boundary_query, 4u);
    const auto boundary_rejected = idx::query_direct_inverted_index(
        view, boundary_query, 3u);
    REQUIRE(boundary_accepted.has_value());
    REQUIRE(boundary_rejected.has_value());
    CHECK(boundary_accepted->results.front().positions_selected);
    CHECK_FALSE(boundary_rejected->results.front().positions_selected);
    CHECK(boundary_rejected->results.front().global_count == 4u);
}

TEST_CASE(
    "direct hash32 codec selects P32 and P64 without narrowing positions",
    "[component:bioseq][layer:unit][feature:index]") {
    SECTION("P32 includes the maximum encoded uint32 position") {
        constexpr std::uint64_t target_length = std::uint64_t{1u} << 31u;
        const std::vector<idx::direct_index_occurrence> input{
            {.hash = 1u, .target_bp = target_length - 1u,
                .target_strand_one = true},
        };
        const auto build_options = options(target_length, 0u, 8u);
        const auto image = build_or_fail(input, build_options);
        const auto view = validate_or_fail(image, build_options.identity);
        CHECK(view.header.posting_width_bytes == 4u);
        const std::array queries{
            idx::direct_index_query_occurrence{
                .hash = 1u, .query_strand_one = false},
        };
        const auto plan = idx::query_direct_inverted_index(view, queries);
        REQUIRE(plan.has_value());
        collected_postings collected{
            .by_selection = std::vector<std::vector<std::uint64_t>>(1u),
        };
        REQUIRE(idx::visit_direct_inverted_index_postings(
            view, *plan, &collected, &collect_posting));
        REQUIRE(collected.by_selection.front().size() == 1u);
        CHECK(collected.by_selection.front().front()
            == (std::numeric_limits<std::uint32_t>::max)());
    }

    SECTION("P64 preserves a position beyond the packed-32 boundary") {
        constexpr std::uint64_t target_length =
            (std::uint64_t{1u} << 31u) + 10u;
        const std::vector<idx::direct_index_occurrence> input{
            {.hash = 1u, .target_bp = (std::uint64_t{1u} << 31u) + 3u,
                .target_strand_one = false},
        };
        const auto build_options = options(target_length, 0u, 8u);
        const auto image = build_or_fail(input, build_options);
        const auto view = validate_or_fail(image, build_options.identity);
        CHECK(view.header.posting_width_bytes == 8u);
        const std::array queries{
            idx::direct_index_query_occurrence{
                .hash = 1u, .query_strand_one = false},
        };
        const auto plan = idx::query_direct_inverted_index(view, queries);
        REQUIRE(plan.has_value());
        collected_postings collected{
            .by_selection = std::vector<std::vector<std::uint64_t>>(1u),
        };
        REQUIRE(idx::visit_direct_inverted_index_postings(
            view, *plan, &collected, &collect_posting));
        REQUIRE(collected.by_selection.front().size() == 1u);
        CHECK(collected.by_selection.front().front()
            == (((std::uint64_t{1u} << 31u) + 3u) << 1u));
    }
}

TEST_CASE(
    "direct hash32 validation fails closed on identity checksum dictionary and "
    "semantic corruption",
    "[component:bioseq][layer:unit][feature:index]") {
    const std::vector<idx::direct_index_occurrence> input{
        {.hash = 1u, .target_bp = 10u, .target_strand_one = false},
        {.hash = 1u, .target_bp = 20u, .target_strand_one = true},
        {.hash = 2u, .target_bp = 30u, .target_strand_one = false},
    };
    const auto build_options = options(100u, 0u, 8u);
    const auto pristine = build_or_fail(input, build_options);

    SECTION("expected identity is exact") {
        auto wrong = build_options.identity;
        ++wrong.source_identity;
        const auto result = idx::validate_direct_inverted_index(pristine, wrong);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error()
            == idx::direct_inverted_index_error::identity_mismatch);
    }

    SECTION("posting corruption is authenticated") {
        auto corrupt = pristine;
        const std::uint64_t postings_offset =
            reference_read_le<std::uint64_t>(corrupt, 144u);
        corrupt[static_cast<std::size_t>(postings_offset)] ^= std::byte{0x40};
        const auto result = idx::validate_direct_inverted_index(
            corrupt, build_options.identity);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error()
            == idx::direct_inverted_index_error::section_checksum);
    }

    SECTION("reserved header bytes remain zero even with a valid header CRC") {
        auto corrupt = pristine;
        corrupt[248u] = std::byte{1};
        reference_reseal_header(corrupt);
        const auto result = idx::validate_direct_inverted_index(
            corrupt, build_options.identity);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error()
            == idx::direct_inverted_index_error::reserved_nonzero);
    }

    SECTION("duplicate dictionary keys fail after checksums are resealed") {
        auto corrupt = pristine;
        const std::uint64_t controls_offset =
            reference_read_le<std::uint64_t>(corrupt, 112u);
        const std::uint32_t first_hash = reference_read_le<std::uint32_t>(
            corrupt, static_cast<std::size_t>(controls_offset));
        reference_write_le<std::uint32_t>(
            corrupt,
            static_cast<std::size_t>(controls_offset
                + idx::k_direct_inverted_index_control_bytes),
            first_hash);
        reference_reseal_controls(corrupt);
        const auto result = idx::validate_direct_inverted_index(
            corrupt, build_options.identity);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == idx::direct_inverted_index_error::dictionary);
    }

    SECTION("authoritative count cannot exceed its posting extent") {
        auto corrupt = pristine;
        const std::uint64_t controls_offset =
            reference_read_le<std::uint64_t>(corrupt, 112u);
        const std::uint64_t first_count = reference_read_le<std::uint64_t>(
            corrupt, static_cast<std::size_t>(controls_offset + 8u));
        reference_write_le<std::uint64_t>(
            corrupt,
            static_cast<std::size_t>(controls_offset + 8u),
            first_count + 1u);
        reference_reseal_controls(corrupt);
        const auto result = idx::validate_direct_inverted_index(
            corrupt, build_options.identity);
        REQUIRE_FALSE(result.has_value());
        CHECK((result.error() == idx::direct_inverted_index_error::control
            || result.error() == idx::direct_inverted_index_error::posting));
    }
}

TEST_CASE(
    "direct hash32 point lookup examines a probe chain rather than every target "
    "control",
    "[component:bioseq][layer:unit][feature:index]") {
    std::vector<idx::direct_index_occurrence> input;
    for (std::uint32_t hash = 0u; hash < 128u; ++hash) {
        input.push_back(idx::direct_index_occurrence{
            .hash = hash,
            .target_bp = hash,
            .target_strand_one = (hash & 1u) != 0u,
        });
    }
    const auto build_options = options(1'000u);
    const auto image = build_or_fail(input, build_options);
    const auto view = validate_or_fail(image, build_options.identity);
    const std::array query{
        idx::direct_index_query_occurrence{
            // mix_hash32(0)==0 and hash zero is inserted first, so this is a
            // deterministic one-control probe rather than a probabilistic
            // assertion about the avalanche mixer's low bits.
            .hash = 0u, .query_strand_one = false},
    };
    const auto plan = idx::query_direct_inverted_index(view, query);
    REQUIRE(plan.has_value());
    REQUIRE(plan->results.front().present);
    CHECK(plan->stats.control_records_examined
        < view.header.unique_hash_count);
    CHECK(plan->stats.dictionary_slots_examined
        < view.header.bucket_count);
}

TEST_CASE(
    "compact exact-domain hash32 geometry freezes canonical K15 and full-hash32 "
    "boundaries and memory economics",
    "[component:bioseq][layer:unit][feature:index]") {
    const auto zero = idx::resolve_compact_hash32_address(
        0u, compact_geometry(32u));
    const auto byte_boundary = idx::resolve_compact_hash32_address(
        8u, compact_geometry(32u));
    const auto rank_left = idx::resolve_compact_hash32_address(
        255u, compact_geometry(32u));
    const auto rank_right = idx::resolve_compact_hash32_address(
        256u, compact_geometry(32u));
    const auto maximum = idx::resolve_compact_hash32_address(
        (std::numeric_limits<std::uint32_t>::max)(),
        compact_geometry(32u));
    REQUIRE(zero.has_value());
    REQUIRE(byte_boundary.has_value());
    REQUIRE(rank_left.has_value());
    REQUIRE(rank_right.has_value());
    REQUIRE(maximum.has_value());
    CHECK(zero->membership_byte_ordinal == 0u);
    CHECK(zero->membership_bit == 0u);
    CHECK(byte_boundary->membership_byte_ordinal == 1u);
    CHECK(byte_boundary->membership_bit == 0u);
    CHECK(rank_left->rank_checkpoint_ordinal == 0u);
    CHECK(rank_left->bit_within_rank_group == 255u);
    CHECK(rank_right->rank_checkpoint_ordinal == 1u);
    CHECK(rank_right->bit_within_rank_group == 0u);
    CHECK(maximum->membership_byte_ordinal
        == idx::k_compact_hash32_full_membership_bytes - 1u);
    CHECK(maximum->membership_bit == 7u);
    CHECK(maximum->rank_checkpoint_ordinal
        == (std::uint64_t{1u} << 24u) - 1u);
    CHECK(maximum->bit_within_rank_group == 255u);

    compact_test_source empty;
    const auto canonical_options = idx::compact_hash32_build_options{
        .identity = identity(1'000u),
        .max_occ_cap = 500u,
    };
    const auto canonical_plan = idx::plan_compact_hash32_direct_index(
        compact_source_ref(empty), canonical_options);
    REQUIRE(canonical_plan.has_value());
    CHECK(canonical_plan->geometry.canonical_k15());
    CHECK(canonical_plan->membership_bytes
        == idx::k_compact_hash32_canonical_k15_membership_bytes);
    CHECK(canonical_plan->rank_bytes
        == idx::k_compact_hash32_canonical_k15_rank_bytes);
    CHECK(canonical_plan->image_bytes == 150'995'200u);

    auto full_options = canonical_options;
    full_options.geometry = compact_geometry(32u);
    const auto full_plan = idx::plan_compact_hash32_direct_index(
        compact_source_ref(empty), full_options);
    REQUIRE(full_plan.has_value());
    CHECK(full_plan->geometry.full_h32());
    CHECK(full_plan->membership_bytes
        == idx::k_compact_hash32_full_membership_bytes);
    CHECK(full_plan->rank_bytes == idx::k_compact_hash32_full_rank_bytes);
    CHECK(full_plan->image_bytes == 603'980'032u);

    constexpr std::uint64_t large_unique_estimate = 357'000'000u;
    constexpr std::uint64_t large_posting_count = 373'218'519u;
    const auto compact_estimate = idx::estimate_compact_hash32_image_bytes(
        canonical_options.geometry,
        large_unique_estimate,
        large_posting_count,
        4u);
    const auto full_estimate = idx::estimate_compact_hash32_image_bytes(
        full_options.geometry,
        large_unique_estimate,
        large_posting_count,
        4u);
    REQUIRE(compact_estimate.has_value());
    REQUIRE(full_estimate.has_value());
    CHECK(compact_estimate->inline_singleton_count == 340'781'481u);
    CHECK(compact_estimate->materialized_hash_count == 16'218'519u);
    CHECK(compact_estimate->materialized_posting_count == 32'437'038u);
    CHECK(compact_estimate->descriptor_bytes == 2'856'000'000u);
    CHECK(compact_estimate->posting_bytes == 129'748'152u);
    CHECK(compact_estimate->image_bytes == 3'136'743'352u);
    CHECK(full_estimate->image_bytes == 3'589'728'184u);

    constexpr std::uint64_t large_query_hash_count = 14'171'377u;
    const auto canonical_touches = idx::estimate_compact_hash32_query_touches(
        large_query_hash_count,
        std::uint64_t{1u} << 22u,
        large_query_hash_count);
    const auto full_touches = idx::estimate_compact_hash32_query_touches(
        large_query_hash_count,
        large_query_hash_count,
        large_query_hash_count);
    REQUIRE(canonical_touches.has_value());
    REQUIRE(full_touches.has_value());
    CHECK(canonical_touches->maximum_membership_bytes == 453'484'064u);
    CHECK(canonical_touches->maximum_rank_bytes == 16'777'216u);
    CHECK(canonical_touches->maximum_descriptor_bytes == 113'371'016u);
    CHECK(canonical_touches->maximum_metadata_bytes == 583'632'296u);
    CHECK(full_touches->maximum_metadata_bytes == 623'540'588u);
}

TEST_CASE(
    "compact hash32 writer binds the persisted cap to uint16 descriptors",
    "[component:bioseq][layer:unit][feature:storage]") {
    compact_test_source empty;
    const auto maximum = compact_options(
        100u,
        (std::numeric_limits<std::uint16_t>::max)(),
        9u);
    const auto accepted = idx::plan_compact_hash32_direct_index(
        compact_source_ref(empty), maximum);
    REQUIRE(accepted.has_value());
    CHECK(accepted->layout_revision
        == idx::k_compact_hash32_direct_index_layout_revision);
    CHECK(accepted->descriptor_width_bytes
        == idx::k_compact_hash32_direct_index_descriptor_bytes);

    auto too_wide = maximum;
    too_wide.max_occ_cap =
        static_cast<std::uint32_t>(
            (std::numeric_limits<std::uint16_t>::max)()) + 1u;
    const auto rejected = idx::plan_compact_hash32_direct_index(
        compact_source_ref(empty), too_wide);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error()
        == idx::direct_inverted_index_error::invalid_options);
    CHECK(empty.scans == 1u);
}

TEST_CASE(
    "compact hash32 bounded open retains a nested nonzero-offset section owner "
    "and exposes a queryable alias-bound view",
    "[component:bioseq][layer:unit][feature:storage]") {
    compact_test_source source{
        .runs = {
            compact_test_run{
                .header = {.hash = 7u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(17u, false)},
            },
        },
    };
    const auto build_options = compact_options(100u, 2u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const std::uint64_t nested_header_receipt_crc64 = reference_crc64(
        std::span<const std::byte>{image}.first(
            idx::k_compact_hash32_direct_index_header_bytes));
    const auto open_options = compact_bounded_options(
        build_options, nested_header_receipt_crc64);

    constexpr std::size_t prefix_bytes = 777u;
    constexpr std::size_t suffix_bytes = 31u;
    std::weak_ptr<std::vector<std::byte>> owner_lifetime;
    {
        auto outer_image = std::make_shared<std::vector<std::byte>>(
            prefix_bytes + image.size() + suffix_bytes, std::byte{0xa5});
        std::copy(image.begin(), image.end(),
            outer_image->begin()
                + static_cast<std::ptrdiff_t>(prefix_bytes));
        owner_lifetime = outer_image;
        const std::span<const std::byte> direct_section{
            outer_image->data() + prefix_bytes, image.size()};
        const std::byte* const outer_image_start = outer_image->data();
        std::shared_ptr<const void> mapped_owner{
            outer_image, outer_image_start};
        REQUIRE(mapped_owner.get() != direct_section.data());

        auto opened = idx::open_compact_hash32_direct_index_bounded(
            std::move(mapped_owner), direct_section, open_options);
        REQUIRE(opened.has_value());
        CHECK(opened->image_owner().get() == direct_section.data());
        CHECK(opened->image_owner().get() != outer_image_start);
        CHECK(opened->view().image.data() == direct_section.data());
        CHECK(opened->view().image.size() == image.size());
        CHECK(opened->view().header.header_crc64
            == reference_read_le<std::uint64_t>(
                image, idx::k_compact_hash32_header_crc_offset));
        CHECK(opened->stats().direct_section_bytes == image.size());
        CHECK(opened->stats().header_bytes_examined
            == idx::k_compact_hash32_direct_index_header_bytes);
        CHECK(opened->stats().payload_bytes_examined == 0u);
        CHECK(opened->stats().expected_direct_header_crc64_checked);

        outer_image.reset();
        CHECK_FALSE(owner_lifetime.expired());
        const std::array query{
            idx::compact_hash32_query_occurrence{
                .hash = 7u, .query_strand_one = false},
        };
        const auto planned = idx::query_compact_hash32_direct_index(
            opened->view(), query, 2u);
        REQUIRE(planned.has_value());
        REQUIRE(planned->results.size() == 1u);
        CHECK(planned->results.front().accepted);
        CHECK(planned->results.front().global_count == 1u);
    }
    CHECK(owner_lifetime.expired());
}

TEST_CASE(
    "compact hash32 bounded open rejects truncated unauthenticated and forged "
    "fixed-header geometry without scanning payload sections",
    "[component:bioseq][layer:unit][feature:storage]") {
    compact_test_source source{
        .runs = {
            compact_test_run{
                .header = {.hash = 7u, .global_count = 2u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {
                    encoded_position(3u, false),
                    encoded_position(9u, true),
                },
            },
        },
    };
    const auto build_options = compact_options(100u, 2u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const auto open_options = compact_bounded_options(build_options);
    const auto open_with_size = [](
            std::vector<std::byte> candidate,
            std::size_t exposed_bytes,
            idx::compact_hash32_bounded_open_options candidate_options) {
        auto owner = std::make_shared<std::vector<std::byte>>(
            std::move(candidate));
        REQUIRE(exposed_bytes <= owner->size());
        const std::span<const std::byte> direct_section{
            owner->data(), exposed_bytes};
        std::shared_ptr<const void> mapped_owner = owner;
        return idx::open_compact_hash32_direct_index_bounded(
            std::move(mapped_owner), direct_section, candidate_options);
    };

    SECTION("a fixed header shorter than 256 bytes is rejected") {
        const auto opened = open_with_size(
            image,
            idx::k_compact_hash32_direct_index_header_bytes - 1u,
            open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::direct_inverted_index_error::image_size);
    }

    SECTION("a section shorter than its authenticated total is rejected") {
        const auto opened = open_with_size(
            image, image.size() - 1u, open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::direct_inverted_index_error::image_size);
    }

    SECTION("a corrupted embedded header checksum is rejected") {
        auto corrupt = image;
        corrupt[idx::k_compact_hash32_header_crc_offset] ^= std::byte{0x01};
        const auto opened = open_compact_bounded_owned(
            std::move(corrupt), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error()
            == idx::direct_inverted_index_error::header_checksum);
    }

    SECTION("an unsupported header revision is rejected exactly") {
        auto forged = image;
        reference_write_le<std::uint32_t>(forged, 8u, 3u);
        reference_reseal_compact_hash32_header(forged);
        const auto opened = open_compact_bounded_owned(
            std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error()
            == idx::direct_inverted_index_error::bad_layout_revision);
    }

    SECTION("an unknown descriptor width is rejected") {
        auto forged = image;
        reference_write_le<std::uint16_t>(forged, 22u, 12u);
        reference_reseal_compact_hash32_header(forged);
        const auto opened = open_compact_bounded_owned(
            std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error()
            == idx::direct_inverted_index_error::bad_header);
    }

    SECTION("an impossible inline-singleton count is rejected") {
        auto forged = image;
        reference_write_le<std::uint64_t>(forged,
            idx::k_compact_hash32_inline_singleton_count_offset, 2u);
        reference_reseal_compact_hash32_header(forged);
        const auto opened = open_compact_bounded_owned(
            std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::direct_inverted_index_error::image_size);
    }

    SECTION("a mismatched outer nested-header receipt is rejected") {
        const std::uint64_t actual_crc = reference_crc64(
            std::span<const std::byte>{image}.first(
                idx::k_compact_hash32_direct_index_header_bytes));
        const auto opened = open_compact_bounded_owned(
            image,
            compact_bounded_options(build_options, actual_crc ^ 1u));
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error()
            == idx::direct_inverted_index_error::receipt_mismatch);
    }

    SECTION("a misaligned membership offset is rejected after resealing") {
        auto forged = image;
        reference_write_le<std::uint64_t>(
            forged, 112u,
            reference_read_le<std::uint64_t>(forged, 112u) + 1u);
        reference_reseal_compact_hash32_header(forged);
        const auto opened = open_compact_bounded_owned(
            std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error()
            == idx::direct_inverted_index_error::section_layout);
    }

    SECTION("overlapping rank and descriptor sections are rejected") {
        auto forged = image;
        reference_write_le<std::uint64_t>(
            forged, 144u,
            reference_read_le<std::uint64_t>(forged, 128u));
        reference_reseal_compact_hash32_header(forged);
        const auto opened = open_compact_bounded_owned(
            std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error()
            == idx::direct_inverted_index_error::section_layout);
    }

    SECTION("an overflowing posting interval is rejected") {
        auto forged = image;
        reference_write_le<std::uint64_t>(
            forged, 160u,
            (std::numeric_limits<std::uint64_t>::max)() - 63u);
        reference_write_le<std::uint64_t>(forged, 168u, 128u);
        reference_reseal_compact_hash32_header(forged);
        const auto opened = open_compact_bounded_owned(
            std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error()
            == idx::direct_inverted_index_error::section_layout);
    }

    SECTION("a forged total direct-section size is rejected") {
        auto forged = image;
        reference_write_le<std::uint64_t>(
            forged, 216u,
            reference_read_le<std::uint64_t>(forged, 216u) + 1u);
        reference_reseal_compact_hash32_header(forged);
        const auto opened = open_compact_bounded_owned(
            std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::direct_inverted_index_error::image_size);
    }

    SECTION("the persisted max-occ cap is matched exactly") {
        auto wrong_options = open_options;
        ++wrong_options.expected_max_occ_cap;
        const auto opened = open_compact_bounded_owned(
            image, wrong_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error()
            == idx::direct_inverted_index_error::identity_mismatch);
    }

    SECTION("the hash-bits and max-hash profile is matched exactly") {
        auto wrong_options = open_options;
        wrong_options.expected_geometry = compact_geometry(10u);
        const auto opened = open_compact_bounded_owned(
            image, wrong_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error()
            == idx::direct_inverted_index_error::identity_mismatch);
    }
}

TEST_CASE(
    "compact hash32 bounded open succeeds when every payload page is no-access",
    "[component:bioseq][layer:unit][feature:storage]") {
#if defined(__unix__) || defined(__APPLE__)
    compact_test_source source{
        .runs = {
            compact_test_run{
                .header = {.hash = 7u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(17u, false)},
            },
        },
    };
    const auto build_options = compact_options(100u, 2u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const std::uint64_t nested_header_receipt_crc64 = reference_crc64(
        std::span<const std::byte>{image}.first(
            idx::k_compact_hash32_direct_index_header_bytes));
    const auto open_options = compact_bounded_options(
        build_options, nested_header_receipt_crc64);

    const long page_size_result = ::sysconf(_SC_PAGESIZE);
    REQUIRE(page_size_result
        > static_cast<long>(idx::k_compact_hash32_direct_index_header_bytes));
    const std::size_t page_size =
        static_cast<std::size_t>(page_size_result);
    const std::size_t payload_bytes =
        image.size() - idx::k_compact_hash32_direct_index_header_bytes;
    const std::size_t payload_pages = (std::max)(
        std::size_t{1u}, (payload_bytes + page_size - 1u) / page_size);
    const std::size_t mapping_bytes = (1u + payload_pages) * page_size;
#if defined(MAP_ANONYMOUS)
    constexpr int anonymous_map_flag = MAP_ANONYMOUS;
#else
    constexpr int anonymous_map_flag = MAP_ANON;
#endif
    void* const mapping = ::mmap(
        nullptr,
        mapping_bytes,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | anonymous_map_flag,
        -1,
        0);
    REQUIRE(mapping != MAP_FAILED);
    std::weak_ptr<const void> mapping_lifetime;
    {
        std::shared_ptr<const void> mapped_owner{
            mapping,
            [mapping_bytes](const void* address) noexcept {
                static_cast<void>(::munmap(
                    const_cast<void*>(address), mapping_bytes));
            },
        };
        mapping_lifetime = mapped_owner;
        auto* const mapping_bytes_begin = static_cast<std::byte*>(mapping);
        std::byte* const direct_section_begin =
            mapping_bytes_begin + page_size
            - idx::k_compact_hash32_direct_index_header_bytes;
        std::memcpy(
            direct_section_begin,
            image.data(),
            idx::k_compact_hash32_direct_index_header_bytes);
        REQUIRE(::mprotect(mapping_bytes_begin, page_size, PROT_READ) == 0);
        REQUIRE(::mprotect(
            mapping_bytes_begin + page_size,
            mapping_bytes - page_size,
            PROT_NONE) == 0);
        const std::span<const std::byte> guarded_direct_section{
            direct_section_begin, image.size()};

        auto opened = idx::open_compact_hash32_direct_index_bounded(
            std::move(mapped_owner), guarded_direct_section, open_options);
        REQUIRE(opened.has_value());
        CHECK(opened->image_owner().get() == direct_section_begin);
        CHECK(opened->view().image.size() == image.size());
        CHECK(opened->view().header.header_crc64
            == reference_read_le<std::uint64_t>(
                image, idx::k_compact_hash32_header_crc_offset));
        CHECK(opened->stats().header_bytes_examined
            == idx::k_compact_hash32_direct_index_header_bytes);
        CHECK(opened->stats().payload_bytes_examined == 0u);
        CHECK_FALSE(mapping_lifetime.expired());
    }
    CHECK(mapping_lifetime.expired());
#else
    SUCCEED("guard-page validation requires a POSIX mmap implementation");
#endif
}

TEST_CASE(
    "compact exact-domain hash32 monotonic batch derives dense ranks and omits "
    "globally rejected max-occ runs",
    "[component:bioseq][layer:unit][feature:index]") {
    compact_test_source source{
        .runs = {
            compact_test_run{
                .header = {.hash = 0u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(20u, false)},
            },
            compact_test_run{
                .header = {.hash = 255u, .global_count = 3u,
                    .target_strand_zero_count = 2u},
                .encoded_positions = {
                    encoded_position(1u, false),
                    encoded_position(9u, false),
                    encoded_position(5u, true),
                },
            },
            compact_test_run{
                .header = {.hash = 256u, .global_count = 2u,
                    .target_strand_zero_count = 0u},
                .encoded_positions = {
                    encoded_position(10u, true),
                    encoded_position(20u, true),
                },
            },
            // Count four exceeds the configured max_occ=3. Neither
            // its membership bit, descriptor, nor postings are persisted.
            compact_test_run{
                .header = {.hash = 300u, .global_count = 4u,
                    .target_strand_zero_count = 2u},
                .encoded_positions = {
                    encoded_position(2u, false),
                    encoded_position(4u, false),
                    encoded_position(6u, true),
                    encoded_position(8u, true),
                },
            },
            compact_test_run{
                .header = {.hash = 511u, .global_count = 3u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {
                    encoded_position(3u, false),
                    encoded_position(7u, true),
                    encoded_position(11u, true),
                },
            },
        },
    };
    const auto build_options = compact_options(100u, 3u, 9u);
    const auto planned = idx::plan_compact_hash32_direct_index(
        compact_source_ref(source), build_options);
    REQUIRE(planned.has_value());
    CHECK(planned->source_hash_count == 5u);
    CHECK(planned->source_posting_count == 13u);
    CHECK(planned->accepted_hash_count == 4u);
    CHECK(planned->accepted_posting_count == 9u);
    CHECK(planned->layout_revision == idx::k_compact_hash32_direct_index_layout_revision);
    CHECK(planned->descriptor_width_bytes
        == idx::k_compact_hash32_direct_index_descriptor_bytes);
    CHECK(planned->inline_singleton_count == 1u);
    CHECK(planned->materialized_hash_count == 3u);
    CHECK(planned->materialized_posting_count == 8u);
    CHECK(planned->rejected_hash_count == 1u);
    CHECK(planned->rejected_posting_count == 4u);
    CHECK(planned->descriptors_bytes == 4u * 8u);
    CHECK(planned->postings_bytes == 8u * 4u);

    const auto image = build_compact_or_fail(source, build_options);
    const auto view = validate_compact_or_fail(image, build_options);
    CHECK(view.header.sorted_stream_crc64 == planned->sorted_stream_crc64);
    const std::vector<idx::compact_hash32_query_occurrence> queries{
        {.hash = 0u, .query_strand_one = false},
        {.hash = 1u, .query_strand_one = false},
        {.hash = 255u, .query_strand_one = false},
        {.hash = 255u, .query_strand_one = true},
        {.hash = 256u, .query_strand_one = false},
        {.hash = 300u, .query_strand_one = false},
        {.hash = 511u, .query_strand_one = true},
    };
    const auto query_plan = idx::query_compact_hash32_direct_index(
        view, queries, 3u);
    REQUIRE(query_plan.has_value());
    REQUIRE(query_plan->results.size() == queries.size());
    CHECK(query_plan->results[0].accepted);
    CHECK(query_plan->results[0].dense_ordinal == 0u);
    CHECK_FALSE(query_plan->results[1].accepted);
    CHECK(query_plan->results[2].accepted);
    CHECK(query_plan->results[2].dense_ordinal == 1u);
    CHECK(query_plan->results[2].forward_count == 2u);
    CHECK(query_plan->results[2].reverse_count == 1u);
    CHECK(query_plan->results[3].selection_ordinal
        == query_plan->results[2].selection_ordinal);
    CHECK(query_plan->results[3].forward_count == 1u);
    CHECK(query_plan->results[3].reverse_count == 2u);
    CHECK(query_plan->results[4].dense_ordinal == 2u);
    CHECK(query_plan->results[4].forward_count == 0u);
    CHECK(query_plan->results[4].reverse_count == 2u);
    CHECK_FALSE(query_plan->results[5].accepted);
    CHECK(query_plan->results[6].dense_ordinal == 3u);
    CHECK(query_plan->stats.rank_checkpoints_examined == 2u);
    CHECK(query_plan->stats.descriptors_examined == 4u);
    CHECK(query_plan->stats.unique_selected_hash_count == 4u);
    CHECK(query_plan->stats.selected_posting_count == 9u);
    CHECK(query_plan->stats.selected_inline_singleton_count == 1u);
    CHECK(query_plan->stats.selected_materialized_hash_count == 3u);
    CHECK(query_plan->stats.selected_materialized_posting_count == 8u);

    collected_postings collected{
        .by_selection = std::vector<std::vector<std::uint64_t>>(
            query_plan->selections.size()),
    };
    const auto visited = idx::visit_compact_hash32_direct_index_postings(
        view, *query_plan, &collected, &collect_posting);
    REQUIRE(visited.has_value());
    CHECK(visited->postings_visited == 9u);
    REQUIRE(collected.by_selection.size() == 4u);
    CHECK(collected.by_selection[0]
        == source.runs[0].encoded_positions);
    CHECK(collected.by_selection[1]
        == source.runs[1].encoded_positions);
    CHECK(collected.by_selection[2]
        == source.runs[2].encoded_positions);
    CHECK(collected.by_selection[3]
        == source.runs[4].encoded_positions);

    const std::array absent_query{
        idx::compact_hash32_query_occurrence{
            .hash = 1u, .query_strand_one = false},
    };
    const auto absent = idx::query_compact_hash32_direct_index(
        view, absent_query, 3u);
    REQUIRE(absent.has_value());
    CHECK_FALSE(absent->results.front().accepted);
    CHECK(absent->stats.rank_checkpoints_examined == 1u);
    CHECK(absent->stats.membership_words_examined == 1u);
    CHECK(absent->stats.descriptors_examined == 0u);

    const std::array unsorted{
        idx::compact_hash32_query_occurrence{
            .hash = 256u, .query_strand_one = false},
        idx::compact_hash32_query_occurrence{
            .hash = 255u, .query_strand_one = false},
    };
    const auto bad_order = idx::query_compact_hash32_direct_index(
        view, unsorted, 3u);
    REQUIRE_FALSE(bad_order.has_value());
    CHECK(bad_order.error() == idx::direct_inverted_index_error::order);
}

TEST_CASE(
    "compact hash32 unique lookup caches one four-word membership rank group "
    "across present absent and boundary hashes",
    "[component:bioseq][layer:unit][feature:index]") {
    compact_test_source source{
        .runs = {
            compact_test_run{
                .header = {.hash = 0u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(1u, false)},
            },
            compact_test_run{
                .header = {.hash = 63u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(2u, false)},
            },
            compact_test_run{
                .header = {.hash = 64u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(3u, false)},
            },
            compact_test_run{
                .header = {.hash = 127u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(4u, false)},
            },
            compact_test_run{
                .header = {.hash = 128u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(5u, false)},
            },
            compact_test_run{
                .header = {.hash = 191u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(6u, false)},
            },
            compact_test_run{
                .header = {.hash = 192u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(7u, false)},
            },
            compact_test_run{
                .header = {.hash = 255u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(8u, false)},
            },
            compact_test_run{
                .header = {.hash = 256u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(9u, false)},
            },
            compact_test_run{
                .header = {.hash = 319u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(10u, false)},
            },
            compact_test_run{
                .header = {.hash = 320u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(11u, false)},
            },
            compact_test_run{
                .header = {.hash = 383u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(12u, false)},
            },
            compact_test_run{
                .header = {.hash = 384u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(13u, false)},
            },
            compact_test_run{
                .header = {.hash = 447u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(14u, false)},
            },
            compact_test_run{
                .header = {.hash = 448u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(15u, false)},
            },
            compact_test_run{
                .header = {.hash = 511u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(16u, false)},
            },
        },
    };
    const auto build_options = compact_options(100u, 1u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const auto view = validate_compact_or_fail(image, build_options);
    const std::array<std::uint32_t, 26u> hashes{
        0u, 1u, 63u, 64u, 65u, 127u, 128u, 129u, 191u, 192u,
        193u, 254u, 255u, 256u, 257u, 319u, 320u, 321u, 383u,
        384u, 385u, 447u, 448u, 449u, 510u, 511u,
    };

    const std::span<const std::uint32_t> first_rank_group{
        hashes.data(), 13u};
    const auto same_group = idx::query_compact_hash32_direct_index_unique(
        view, first_rank_group, 1u);
    REQUIRE(same_group.has_value());
    CHECK(same_group->selections.size() == 8u);
    CHECK(same_group->stats.rank_checkpoints_examined == 1u);
    // Thirteen present/absent lookups physically load exactly one cached
    // four-word group, not one to four words again for every hash.
    CHECK(same_group->stats.membership_words_examined == 4u);
    CHECK(same_group->stats.descriptors_examined == 8u);

    const auto cached = idx::query_compact_hash32_direct_index_unique(
        view, hashes, 1u);
    REQUIRE(cached.has_value());
    CHECK(cached->selections.size() == source.runs.size());
    CHECK(cached->stats.rank_checkpoints_examined == 2u);
    CHECK(cached->stats.membership_words_examined == 8u);
    CHECK(cached->stats.descriptors_examined == source.runs.size());
    REQUIRE(cached->selections.size() == 16u);
    CHECK(cached->selections[7u].query_ordinal == 12u);
    CHECK(cached->selections[7u].dense_ordinal == 7u);
    CHECK(cached->selections[8u].query_ordinal == 13u);
    CHECK(cached->selections[8u].dense_ordinal == 8u);

    struct projected_hash_record {
        std::uint32_t hash = 0u;
        std::uint32_t occurrence_ref_begin = 0u;
        std::uint32_t occurrence_ref_count = 0u;
        std::uint32_t global_occurrence_count = 0u;
    };
    static_assert(sizeof(projected_hash_record) == 16u);
    std::array<projected_hash_record, 26u> projected_hashes{};
    for (std::size_t ordinal = 0u; ordinal < hashes.size(); ++ordinal) {
        projected_hashes[ordinal] = projected_hash_record{
            .hash = hashes[ordinal],
            .occurrence_ref_begin = static_cast<std::uint32_t>(ordinal),
            .occurrence_ref_count = static_cast<std::uint32_t>(ordinal + 1u),
            .global_occurrence_count = static_cast<std::uint32_t>(
                ordinal + 2u),
        };
    }
    std::size_t projected_hash_reads = 0u;
    const auto projected =
        idx::query_compact_hash32_direct_index_unique_projected(
            view,
            projected_hashes.size(),
            [&projected_hashes, &projected_hash_reads](
                    std::size_t ordinal) noexcept
                    -> std::uint32_t {
                ++projected_hash_reads;
                return projected_hashes[ordinal].hash;
            },
            1u);
    REQUIRE(projected.has_value());
    CHECK(projected_hash_reads == projected_hashes.size());
    CHECK(projected->stats == cached->stats);
    REQUIRE(projected->selections.size() == cached->selections.size());
    for (std::size_t ordinal = 0u;
         ordinal < cached->selections.size(); ++ordinal) {
        CHECK(projected->selections[ordinal].query_ordinal
            == cached->selections[ordinal].query_ordinal);
        CHECK(projected->selections[ordinal].dense_ordinal
            == cached->selections[ordinal].dense_ordinal);
    }
    const auto projected_accessor =
        idx::bind_compact_hash32_direct_index_selection_spans(
            view, *projected);
    REQUIRE(projected_accessor.has_value());
    CHECK(projected_accessor->selection_count()
        == projected->selections.size());

    std::vector<idx::compact_hash32_query_occurrence> occurrence_queries;
    occurrence_queries.reserve(hashes.size());
    for (const std::uint32_t hash : hashes) {
        occurrence_queries.push_back(idx::compact_hash32_query_occurrence{
            .hash = hash,
            .query_strand_one = false,
        });
    }
    const auto reference = idx::query_compact_hash32_direct_index(
        view, occurrence_queries, 1u);
    REQUIRE(reference.has_value());
    REQUIRE(reference->results.size() == hashes.size());
    std::size_t selected_ordinal = 0u;
    for (std::size_t query_ordinal = 0u;
         query_ordinal < hashes.size(); ++query_ordinal) {
        if (!reference->results[query_ordinal].accepted) continue;
        REQUIRE(selected_ordinal < cached->selections.size());
        CHECK(cached->selections[selected_ordinal].query_ordinal
            == query_ordinal);
        CHECK(cached->selections[selected_ordinal].dense_ordinal
            == reference->results[query_ordinal].dense_ordinal);
        ++selected_ordinal;
    }
    CHECK(selected_ordinal == cached->selections.size());
}

TEST_CASE(
    "compact exact-domain hash32 codec preserves P64 accepted postings",
    "[component:bioseq][layer:unit][feature:storage]") {
    constexpr std::uint64_t target_length =
        (std::uint64_t{1u} << 31u) + 100u;
    compact_test_source source{
        .runs = {
            compact_test_run{
                .header = {.hash = 2u, .global_count = 1u,
                    .target_strand_zero_count = 0u},
                .encoded_positions = {
                    encoded_position(
                        (std::uint64_t{1u} << 31u) + 7u, true),
                },
            },
        },
    };
    const auto build_options = compact_options(target_length, 1u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const auto view = validate_compact_or_fail(image, build_options);
    CHECK(view.header.layout_revision
        == idx::k_compact_hash32_direct_index_layout_revision);
    CHECK(view.header.descriptor_width_bytes
        == idx::k_compact_hash32_direct_index_descriptor_bytes);
    CHECK(view.header.posting_width_bytes == 8u);
    CHECK(view.header.inline_singleton_count == 0u);
    CHECK(view.header.materialized_hash_count == 1u);
    CHECK(view.header.materialized_posting_count == 1u);
    const std::array query{
        idx::compact_hash32_query_occurrence{
            .hash = 2u, .query_strand_one = false},
    };
    const auto plan = idx::query_compact_hash32_direct_index(view, query, 1u);
    REQUIRE(plan.has_value());
    collected_postings collected{
        .by_selection = std::vector<std::vector<std::uint64_t>>(1u),
    };
    const auto visited = idx::visit_compact_hash32_direct_index_postings(
        view, *plan, &collected, &collect_posting);
    REQUIRE(visited.has_value());
    CHECK(collected.by_selection.front()
        == source.runs.front().encoded_positions);

    const std::array<std::uint32_t, 1u> unique_hashes{2u};
    const auto unique_plan = idx::query_compact_hash32_direct_index_unique(
        view, unique_hashes, 1u);
    REQUIRE(unique_plan.has_value());
    REQUIRE(unique_plan->selections.size() == 1u);
    CHECK(unique_plan->selections.front().dense_ordinal == 0u);
    CHECK(unique_plan->selections.front().query_ordinal == 0u);
    CHECK(unique_plan->stats.selected_inline_singleton_count == 0u);
    CHECK(unique_plan->stats.selected_materialized_hash_count == 1u);
    CHECK(unique_plan->stats.selected_materialized_posting_count == 1u);
    struct projected_position64_hash_record {
        std::uint32_t hash = 0u;
        std::uint32_t occurrence_ref_begin = 0u;
        std::uint32_t occurrence_ref_count = 0u;
        std::uint32_t global_occurrence_count = 0u;
    };
    static_assert(sizeof(projected_position64_hash_record) == 16u);
    const std::array projected_hashes{
        projected_position64_hash_record{
            .hash = 2u,
            .occurrence_ref_begin = 3u,
            .occurrence_ref_count = 5u,
            .global_occurrence_count = 7u,
        },
    };
    std::size_t projected_hash_reads = 0u;
    const auto projected_plan =
        idx::query_compact_hash32_direct_index_unique_projected(
            view,
            projected_hashes.size(),
            [&projected_hashes, &projected_hash_reads](
                    std::size_t ordinal) noexcept
                    -> std::uint32_t {
                ++projected_hash_reads;
                return projected_hashes[ordinal].hash;
            },
            1u);
    REQUIRE(projected_plan.has_value());
    CHECK(projected_hash_reads == projected_hashes.size());
    CHECK(projected_plan->stats == unique_plan->stats);
    REQUIRE(projected_plan->selections.size()
        == unique_plan->selections.size());
    CHECK(projected_plan->selections.front().dense_ordinal
        == unique_plan->selections.front().dense_ordinal);
    CHECK(projected_plan->selections.front().query_ordinal
        == unique_plan->selections.front().query_ordinal);
    const auto projected_p64_accessor =
        idx::bind_compact_hash32_direct_index_selection_spans(
            view, *projected_plan);
    REQUIRE(projected_p64_accessor.has_value());
    CHECK(projected_p64_accessor->selection_count() == 1u);
    std::uint64_t selection_callback_count = 0u;
    std::vector<std::uint64_t> bulk_positions;
    bool span_shape_valid = true;
    const auto bulk_visited =
        idx::visit_compact_hash32_direct_index_selection_spans(
            view,
            *unique_plan,
            [&](const idx::compact_hash32_selection_posting_spans& selected) {
                ++selection_callback_count;
                if (selected.selection_ordinal != 0u
                    || selected.dense_ordinal != 0u
                    || selected.query_ordinal != 0u
                    || selected.global_count != 1u
                    || selected.strand_zero_count != 0u
                    || selected.span_count != 1u) {
                    span_shape_valid = false;
                    return false;
                }
                const auto& span = selected.spans.front();
                if (span.count != 1u
                    || span.posting_width_bytes != 8u
                    || span.encoded_bytes.size() != 8u
                    || !span.target_strand_one
                    || span.target_length != target_length) {
                    span_shape_valid = false;
                    return false;
                }
                const auto encoded = span.encoded_position_at(0u);
                if (!encoded) {
                    span_shape_valid = false;
                    return false;
                }
                bulk_positions.push_back(*encoded);
                return true;
            });
    CHECK(span_shape_valid);
    REQUIRE(bulk_visited.has_value());
    CHECK(selection_callback_count == 1u);
    CHECK(bulk_visited->selection_callbacks == 1u);
    CHECK(bulk_visited->spans_exposed == 1u);
    CHECK(bulk_visited->postings_exposed == 1u);
    CHECK(bulk_visited->inline_selection_callbacks == 0u);
    CHECK(bulk_visited->materialized_selection_callbacks == 1u);
    CHECK(bulk_visited->inline_postings_exposed == 0u);
    CHECK(bulk_visited->materialized_postings_exposed == 1u);
    CHECK(bulk_visited->descriptors_examined == 1u);
    CHECK(bulk_visited->rank_checkpoints_examined == 0u);
    CHECK(bulk_visited->membership_words_examined == 0u);
    CHECK(bulk_positions == source.runs.front().encoded_positions);

    const auto p64_range_accessor =
        idx::bind_compact_hash32_direct_index_selection_spans(
            view, *unique_plan);
    REQUIRE(p64_range_accessor.has_value());
    std::vector<std::uint64_t> p64_range_positions;
    std::optional<idx::compact_hash32_posting_span_view> p64_span;
    const auto p64_range_visited =
        idx::visit_compact_hash32_direct_index_selection_span_range(
            *p64_range_accessor,
            0u,
            1u,
            [&](const idx::compact_hash32_selection_posting_spans& selected) {
                if (selected.selection_ordinal != 0u
                    || selected.span_count != 1u
                    || selected.spans.front().posting_width_bytes != 8u) {
                    return false;
                }
                p64_span = selected.spans.front();
                const auto decoded =
                    selected.spans.front().visit_encoded_positions(
                        [&](std::uint64_t encoded) {
                            p64_range_positions.push_back(encoded);
                            return true;
                        });
                return decoded.has_value() && *decoded == 1u;
            });
    REQUIRE(p64_range_visited.has_value());
    CHECK(p64_range_visited->selection_callbacks == 1u);
    CHECK(p64_range_visited->postings_exposed == 1u);
    CHECK(p64_range_visited->materialized_postings_exposed == 1u);
    CHECK(p64_range_positions == bulk_positions);
    REQUIRE(p64_span.has_value());

    auto truncated_p64_span = *p64_span;
    truncated_p64_span.encoded_bytes =
        truncated_p64_span.encoded_bytes.first(7u);
    std::uint64_t truncated_callbacks = 0u;
    const auto truncated_p64 =
        truncated_p64_span.visit_encoded_positions(
            [&](std::uint64_t) {
                ++truncated_callbacks;
                return true;
            });
    REQUIRE_FALSE(truncated_p64.has_value());
    CHECK(truncated_p64.error()
        == idx::direct_inverted_index_error::query_plan);
    CHECK(truncated_callbacks == 0u);

    auto wrong_strand_p64_span = *p64_span;
    wrong_strand_p64_span.target_strand_one = false;
    std::uint64_t wrong_strand_callbacks = 0u;
    const auto wrong_strand_p64 =
        wrong_strand_p64_span.visit_encoded_positions(
            [&](std::uint64_t) {
                ++wrong_strand_callbacks;
                return true;
            });
    REQUIRE_FALSE(wrong_strand_p64.has_value());
    CHECK(wrong_strand_p64.error()
        == idx::direct_inverted_index_error::query_plan);
    CHECK(wrong_strand_callbacks == 0u);

    auto out_of_range_p64_span = *p64_span;
    out_of_range_p64_span.target_length = 1u;
    std::uint64_t out_of_range_callbacks = 0u;
    const auto out_of_range_p64 =
        out_of_range_p64_span.visit_encoded_positions(
            [&](std::uint64_t) {
                ++out_of_range_callbacks;
                return true;
            });
    REQUIRE_FALSE(out_of_range_p64.has_value());
    CHECK(out_of_range_p64.error()
        == idx::direct_inverted_index_error::query_plan);
    CHECK(out_of_range_callbacks == 0u);

    std::uint64_t rejected_callbacks = 0u;
    const auto rejected_p64 = p64_span->visit_encoded_positions(
        [&](std::uint64_t) {
            ++rejected_callbacks;
            return false;
        });
    REQUIRE_FALSE(rejected_p64.has_value());
    CHECK(rejected_p64.error()
        == idx::direct_inverted_index_error::visitor_rejected);
    CHECK(rejected_callbacks == 1u);
}

TEST_CASE(
    "compact hash32 reader rejects forged descriptor interpretation after "
    "payload checksums are resealed",
    "[component:bioseq][layer:unit][feature:storage]") {
    compact_test_source source{
        .runs = {
            compact_test_run{
                .header = {.hash = 2u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(4u, false)},
            },
            compact_test_run{
                .header = {.hash = 5u, .global_count = 2u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {
                    encoded_position(6u, false),
                    encoded_position(8u, true),
                },
            },
        },
    };
    const auto build_options = compact_options(100u, 2u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const std::uint64_t descriptors_offset =
        reference_read_le<std::uint64_t>(image, 144u);

    SECTION("an inline singleton cannot forge its persisted strand") {
        auto forged = image;
        reference_write_le<std::uint32_t>(forged, descriptors_offset,
            static_cast<std::uint32_t>(encoded_position(4u, true)));
        reference_reseal_compact_hash32_payload_and_header(forged);
        const auto validated = idx::validate_compact_hash32_direct_index(
            forged,
            build_options.identity,
            build_options.max_occ_cap,
            build_options.geometry);
        REQUIRE_FALSE(validated.has_value());
        CHECK(validated.error()
            == idx::direct_inverted_index_error::posting);
    }

    SECTION("a materialized descriptor cannot be retyped as an extra inline") {
        auto forged = image;
        const std::uint64_t second_descriptor = descriptors_offset
            + idx::k_compact_hash32_direct_index_descriptor_bytes;
        reference_write_le<std::uint16_t>(
            forged, second_descriptor + 4u, 1u);
        reference_reseal_compact_hash32_payload_and_header(forged);
        const auto validated = idx::validate_compact_hash32_direct_index(
            forged,
            build_options.identity,
            build_options.max_occ_cap,
            build_options.geometry);
        REQUIRE_FALSE(validated.has_value());
        CHECK(validated.error()
            == idx::direct_inverted_index_error::dictionary);
    }

    SECTION("a materialized posting ordinal remains exact") {
        auto forged = image;
        const std::uint64_t second_descriptor = descriptors_offset
            + idx::k_compact_hash32_direct_index_descriptor_bytes;
        reference_write_le<std::uint32_t>(forged, second_descriptor, 99u);
        reference_reseal_compact_hash32_payload_and_header(forged);
        const auto validated = idx::validate_compact_hash32_direct_index(
            forged,
            build_options.identity,
            build_options.max_occ_cap,
            build_options.geometry);
        REQUIRE_FALSE(validated.has_value());
        CHECK(validated.error()
            == idx::direct_inverted_index_error::control);
    }
}

TEST_CASE(
    "compact exact-domain hash32 rejects a forged image plan before touching "
    "the destination",
    "[component:bioseq][layer:unit][feature:storage]") {
    compact_test_source source{
        .runs = {
            compact_test_run{
                .header = {.hash = 7u, .global_count = 2u,
                    .target_strand_zero_count = 2u},
                .encoded_positions = {
                    encoded_position(3u, false),
                    encoded_position(9u, false),
                },
            },
        },
    };
    const auto build_options = compact_options(100u, 2u, 9u);
    const auto planned = idx::plan_compact_hash32_direct_index(
        compact_source_ref(source), build_options);
    REQUIRE(planned.has_value());
    auto forged = *planned;
    forged.descriptors_offset += 64u;

    constexpr std::byte canary{0xa5};
    std::vector<std::byte> guarded(
        static_cast<std::size_t>(planned->image_bytes) + 32u, canary);
    const auto destination = std::span<std::byte>{guarded}.first(
        static_cast<std::size_t>(planned->image_bytes));
    const auto built = idx::build_compact_hash32_direct_index_into(
        compact_source_ref(source), build_options, forged, destination);
    REQUIRE_FALSE(built.has_value());
    CHECK(built.error()
        == idx::direct_inverted_index_error::storage_contract_mismatch);

    CHECK(source.scans == 1u);
    CHECK(std::all_of(guarded.begin(), guarded.end(),
        [canary](std::byte value) { return value == canary; }));
}

TEST_CASE(
    "compact exact-domain hash32 bounds flapping replay sources before every "
    "planned section write",
    "[component:bioseq][layer:unit][feature:storage]") {
    const auto build_options = compact_options(100u, 3u, 9u);
    const auto require_replay_rejected = [&build_options](
            compact_test_source& source) {
        const auto planned = idx::plan_compact_hash32_direct_index(
            compact_source_ref(source), build_options);
        REQUIRE(planned.has_value());
        constexpr std::byte canary{0xa5};
        std::vector<std::byte> guarded(
            static_cast<std::size_t>(planned->image_bytes) + 32u, canary);
        const auto destination = std::span<std::byte>{guarded}.first(
            static_cast<std::size_t>(planned->image_bytes));
        const auto built = idx::build_compact_hash32_direct_index_into(
            compact_source_ref(source), build_options, *planned,
            destination);
        REQUIRE_FALSE(built.has_value());
        CHECK(built.error()
            == idx::direct_inverted_index_error::replay_mismatch);
        CHECK(source.scans == 2u);
        const auto tail = std::span<const std::byte>{guarded}.subspan(
            static_cast<std::size_t>(planned->image_bytes));
        CHECK(std::all_of(tail.begin(), tail.end(),
            [canary](std::byte value) { return value == canary; }));
    };

    SECTION("replay adds an accepted run") {
        compact_test_source source{
            .runs = {
                compact_test_run{
                    .header = {.hash = 20u, .global_count = 1u,
                        .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(2u, false)},
                },
            },
            .replay_runs = {
                compact_test_run{
                    .header = {.hash = 20u, .global_count = 1u,
                        .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(2u, false)},
                },
                compact_test_run{
                    .header = {.hash = 30u, .global_count = 1u,
                        .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(3u, false)},
                },
            },
            .use_replay_runs_after_first_scan = true,
        };
        require_replay_rejected(source);
    }

    SECTION("replay removes an accepted run") {
        compact_test_source source{
            .runs = {
                compact_test_run{
                    .header = {.hash = 20u, .global_count = 1u,
                        .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(2u, false)},
                },
            },
            .replay_runs = {},
            .use_replay_runs_after_first_scan = true,
        };
        require_replay_rejected(source);
    }

    SECTION("replay changes an accepted posting count") {
        compact_test_source source{
            .runs = {
                compact_test_run{
                    .header = {.hash = 20u, .global_count = 1u,
                        .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(2u, false)},
                },
            },
            .replay_runs = {
                compact_test_run{
                    .header = {.hash = 20u, .global_count = 2u,
                        .target_strand_zero_count = 2u},
                    .encoded_positions = {
                        encoded_position(2u, false),
                        encoded_position(4u, false),
                    },
                },
            },
            .use_replay_runs_after_first_scan = true,
        };
        require_replay_rejected(source);
    }

    SECTION("replay changes a hash without changing section sizes") {
        compact_test_source source{
            .runs = {
                compact_test_run{
                    .header = {.hash = 20u, .global_count = 1u,
                        .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(2u, false)},
                },
            },
            .replay_runs = {
                compact_test_run{
                    .header = {.hash = 21u, .global_count = 1u,
                        .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(2u, false)},
                },
            },
            .use_replay_runs_after_first_scan = true,
        };
        require_replay_rejected(source);
    }
}

TEST_CASE(
    "compact exact-domain hash32 point lookup decodes one descriptor among many",
    "[component:bioseq][layer:unit][feature:index]") {
    compact_test_source source;
    for (std::uint32_t hash = 0u; hash < 64u; ++hash) {
        source.runs.push_back(compact_test_run{
            .header = {.hash = hash, .global_count = 1u,
                .target_strand_zero_count = 1u},
            .encoded_positions = {encoded_position(hash, false)},
        });
    }
    const auto build_options = compact_options(100u, 1u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const auto view = validate_compact_or_fail(image, build_options);
    CHECK(view.header.layout_revision
        == idx::k_compact_hash32_direct_index_layout_revision);
    CHECK(view.header.descriptor_width_bytes
        == idx::k_compact_hash32_direct_index_descriptor_bytes);
    CHECK(view.header.inline_singleton_count == 64u);
    CHECK(view.header.materialized_hash_count == 0u);
    CHECK(view.header.materialized_posting_count == 0u);
    CHECK(view.header.postings_bytes == 0u);
    REQUIRE(view.header.accepted_hash_count == 64u);

    const std::array present_query{
        idx::compact_hash32_query_occurrence{
            .hash = 63u, .query_strand_one = false},
    };
    const auto present = idx::query_compact_hash32_direct_index(
        view, present_query, 1u);
    REQUIRE(present.has_value());
    REQUIRE(present->results.front().accepted);
    CHECK(present->results.front().dense_ordinal == 63u);
    CHECK(present->stats.descriptors_examined == 1u);
    CHECK(present->stats.unique_selected_hash_count == 1u);

    const std::array absent_query{
        idx::compact_hash32_query_occurrence{
            .hash = 64u, .query_strand_one = false},
    };
    const auto absent = idx::query_compact_hash32_direct_index(
        view, absent_query, 1u);
    REQUIRE(absent.has_value());
    CHECK_FALSE(absent->results.front().accepted);
    CHECK(absent->stats.descriptors_examined == 0u);
    CHECK(absent->stats.unique_selected_hash_count == 0u);
}

TEST_CASE(
    "compact exact-domain hash32 applies runtime max-occ below the persisted cap "
    "without retaining or reading rejected postings under exact U+c admission",
    "[component:bioseq][layer:unit][feature:index]") {
    STATIC_REQUIRE(sizeof(idx::compact_hash32_unique_selection) == 8u);
    compact_test_source source{
        .runs = {
            compact_test_run{
                .header = {.hash = 2u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(2u, false)},
            },
            compact_test_run{
                .header = {.hash = 5u, .global_count = 3u,
                    .target_strand_zero_count = 2u},
                .encoded_positions = {
                    encoded_position(5u, false),
                    encoded_position(7u, false),
                    encoded_position(9u, true),
                },
            },
            compact_test_run{
                .header = {.hash = 7u, .global_count = 4u,
                    .target_strand_zero_count = 2u},
                .encoded_positions = {
                    encoded_position(10u, false),
                    encoded_position(12u, false),
                    encoded_position(14u, true),
                    encoded_position(16u, true),
                },
            },
        },
    };
    const auto build_options = compact_options(100u, 4u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const auto view = validate_compact_or_fail(image, build_options);
    CHECK(view.header.layout_revision
        == idx::k_compact_hash32_direct_index_layout_revision);
    CHECK(view.header.descriptor_width_bytes
        == idx::k_compact_hash32_direct_index_descriptor_bytes);
    CHECK(view.header.inline_singleton_count == 1u);
    CHECK(view.header.materialized_hash_count == 2u);
    CHECK(view.header.materialized_posting_count == 7u);

    const std::array queries{
        idx::compact_hash32_query_occurrence{
            .hash = 1u, .query_strand_one = false},
        idx::compact_hash32_query_occurrence{
            .hash = 2u, .query_strand_one = false},
        idx::compact_hash32_query_occurrence{
            .hash = 5u, .query_strand_one = false},
        idx::compact_hash32_query_occurrence{
            .hash = 7u, .query_strand_one = false},
    };
    const auto lower = idx::query_compact_hash32_direct_index(
        view, queries, 2u);
    REQUIRE(lower.has_value());
    REQUIRE(lower->results.size() == queries.size());
    CHECK_FALSE(lower->results[0].accepted);
    CHECK(lower->results[1].accepted);
    CHECK_FALSE(lower->results[2].accepted);
    CHECK_FALSE(lower->results[3].accepted);
    CHECK(lower->selections.size() == 1u);
    CHECK(lower->stats.runtime_rejected_hash_count == 2u);
    CHECK(lower->stats.runtime_rejected_posting_count == 7u);
    CHECK(lower->stats.selected_posting_count == 1u);
    CHECK(lower->stats.rejected_posting_reads == 0u);

    const std::array<std::uint32_t, 4u> unique_hashes{1u, 2u, 5u, 7u};
    const auto sparse = idx::query_compact_hash32_direct_index_unique(
        view, unique_hashes, 2u);
    REQUIRE(sparse.has_value());
    REQUIRE(sparse->selections.size() == 1u);
    CHECK(sparse->selections.front().dense_ordinal == 0u);
    CHECK(sparse->selections.front().query_ordinal == 1u);
    CHECK(sparse->retained_selection_capacity_bytes()
        == sparse->selections.capacity()
            * sizeof(idx::compact_hash32_unique_selection));
    CHECK(sparse->selections.capacity() == 1u);
    CHECK(sparse->stats.selection_reserve_call_count == 1u);
    CHECK(sparse->stats.selection_reserve_requested_elements == 4u);
    CHECK(sparse->stats.selection_initial_capacity_elements == 4u);
    CHECK(sparse->stats.selection_fill_reallocation_count == 0u);
    CHECK(sparse->stats.selection_compaction_allocation_count == 1u);
    CHECK(sparse->stats.selection_storage_allocation_count == 2u);
    CHECK(sparse->stats.selection_peak_capacity_bytes
        == 5u * sizeof(idx::compact_hash32_unique_selection));
    CHECK(sparse->stats.selection_retained_capacity_elements == 1u);
    CHECK(sparse->stats.selection_retained_capacity_bytes
        == sizeof(idx::compact_hash32_unique_selection));
    CHECK(sparse->stats.runtime_rejected_hash_count == 2u);
    CHECK(sparse->stats.runtime_rejected_posting_count == 7u);
    CHECK(sparse->stats.selected_posting_count == 1u);
    CHECK(sparse->stats.selected_inline_singleton_count == 1u);
    CHECK(sparse->stats.selected_materialized_hash_count == 0u);
    CHECK(sparse->stats.selected_materialized_posting_count == 0u);
    CHECK(sparse->stats.rejected_posting_reads == 0u);

    collected_postings collected{
        .by_selection = std::vector<std::vector<std::uint64_t>>(1u),
    };
    const auto visited = idx::visit_compact_hash32_direct_index_postings(
        view, *sparse, &collected, &collect_posting);
    REQUIRE(visited.has_value());
    CHECK(visited->postings_visited == 1u);
    CHECK(collected.callback_count == 1u);
    CHECK(collected.by_selection.front()
        == source.runs.front().encoded_positions);

    std::uint64_t sparse_selection_callbacks = 0u;
    const auto sparse_bulk_visited =
        idx::visit_compact_hash32_direct_index_selection_spans(
            view,
            *sparse,
            [&](const idx::compact_hash32_selection_posting_spans& selected) {
                ++sparse_selection_callbacks;
                return selected.selection_ordinal == 0u
                    && selected.dense_ordinal == 0u
                    && selected.query_ordinal == 1u
                    && selected.global_count == 1u
                    && selected.strand_zero_count == 1u
                    && selected.span_count == 1u;
            });
    REQUIRE(sparse_bulk_visited.has_value());
    CHECK(sparse_selection_callbacks == 1u);
    CHECK(sparse_bulk_visited->selection_callbacks == 1u);
    CHECK(sparse_bulk_visited->spans_exposed == 1u);
    CHECK(sparse_bulk_visited->postings_exposed == 1u);
    CHECK(sparse_bulk_visited->inline_selection_callbacks == 1u);
    CHECK(sparse_bulk_visited->materialized_selection_callbacks == 0u);
    CHECK(sparse_bulk_visited->inline_postings_exposed == 1u);
    CHECK(sparse_bulk_visited->materialized_postings_exposed == 0u);
    CHECK(sparse_bulk_visited->descriptors_examined == 1u);
    CHECK(sparse_bulk_visited->rank_checkpoints_examined == 0u);
    CHECK(sparse_bulk_visited->membership_words_examined == 0u);

    const auto equal = idx::query_compact_hash32_direct_index_unique(
        view, unique_hashes, 4u);
    REQUIRE(equal.has_value());
    CHECK(equal->selections.size() == 3u);
    CHECK(equal->stats.runtime_rejected_hash_count == 0u);
    CHECK(equal->stats.runtime_rejected_posting_count == 0u);
    CHECK(equal->stats.selected_posting_count == 8u);
    CHECK(equal->stats.selected_inline_singleton_count == 1u);
    CHECK(equal->stats.selected_materialized_hash_count == 2u);
    CHECK(equal->stats.selected_materialized_posting_count == 7u);
    CHECK(equal->stats.rejected_posting_reads == 0u);
    CHECK(equal->selections.size() == 3u);
    CHECK(equal->selections.capacity() == 3u);
    CHECK(equal->stats.selection_reserve_requested_elements == 4u);
    CHECK(equal->stats.selection_initial_capacity_elements == 4u);
    CHECK(equal->stats.selection_fill_reallocation_count == 0u);
    CHECK(equal->stats.selection_compaction_allocation_count == 1u);
    CHECK(equal->stats.selection_storage_allocation_count == 2u);
    CHECK(equal->stats.selection_peak_capacity_bytes
        == 7u * sizeof(idx::compact_hash32_unique_selection));
    CHECK(equal->stats.selection_retained_capacity_elements == 3u);
    CHECK(equal->stats.selection_retained_capacity_bytes
        == 3u * sizeof(idx::compact_hash32_unique_selection));
    REQUIRE(equal->selections.size() == 3u);
    CHECK(equal->selections[0].dense_ordinal == 0u);
    CHECK(equal->selections[0].query_ordinal == 1u);
    CHECK(equal->selections[1].dense_ordinal == 1u);
    CHECK(equal->selections[1].query_ordinal == 2u);
    CHECK(equal->selections[2].dense_ordinal == 2u);
    CHECK(equal->selections[2].query_ordinal == 3u);
    // Returned capacity is exactly c: a hypothetical c+1 push would need a
    // growth, while all c query-time pushes provably performed none.
    CHECK(equal->selections.size() + 1u > equal->selections.capacity());

    std::vector<std::tuple<std::uint32_t, std::uint32_t,
        std::uint32_t, std::uint32_t, std::uint32_t, std::uint8_t>>
        callback_order;
    std::vector<std::vector<std::uint64_t>> bulk_postings(
        equal->selections.size());
    std::vector<std::vector<bool>> bulk_span_strands(
        equal->selections.size());
    bool all_bulk_positions_decoded = true;
    const auto bulk_visited =
        idx::visit_compact_hash32_direct_index_selection_spans(
            view,
            *equal,
            [&](const idx::compact_hash32_selection_posting_spans& selected) {
                callback_order.emplace_back(
                    selected.selection_ordinal,
                    selected.dense_ordinal,
                    selected.query_ordinal,
                    selected.global_count,
                    selected.strand_zero_count,
                    selected.span_count);
                if (selected.selection_ordinal
                        >= bulk_postings.size()) {
                    return false;
                }
                for (std::uint8_t span_ordinal = 0u;
                     span_ordinal < selected.span_count;
                     ++span_ordinal) {
                    const auto& span = selected.spans[span_ordinal];
                    bulk_span_strands[selected.selection_ordinal].push_back(
                        span.target_strand_one);
                    if (span.encoded_bytes.size()
                            != span.count * span.posting_width_bytes) {
                        return false;
                    }
                    for (std::uint64_t ordinal = 0u;
                         ordinal < span.count;
                         ++ordinal) {
                        const auto encoded =
                            span.encoded_position_at(ordinal);
                        if (!encoded) {
                            all_bulk_positions_decoded = false;
                            return false;
                        }
                        bulk_postings[selected.selection_ordinal]
                            .push_back(*encoded);
                    }
                    CHECK_FALSE(
                        span.encoded_position_at(span.count).has_value());
                }
                return true;
            });
    CHECK(all_bulk_positions_decoded);
    REQUIRE(bulk_visited.has_value());
    CHECK(bulk_visited->selection_callbacks == 3u);
    CHECK(bulk_visited->spans_exposed == 5u);
    CHECK(bulk_visited->postings_exposed == 8u);
    CHECK(bulk_visited->inline_selection_callbacks == 1u);
    CHECK(bulk_visited->materialized_selection_callbacks == 2u);
    CHECK(bulk_visited->inline_postings_exposed == 1u);
    CHECK(bulk_visited->materialized_postings_exposed == 7u);
    CHECK(bulk_visited->descriptors_examined == 3u);
    CHECK(bulk_visited->rank_checkpoints_examined == 0u);
    CHECK(bulk_visited->membership_words_examined == 0u);
    CHECK(equal->stats.rank_checkpoints_examined != 0u);
    CHECK(equal->stats.membership_words_examined != 0u);
    CHECK((callback_order == decltype(callback_order){
        {0u, 0u, 1u, 1u, 1u, 1u},
        {1u, 1u, 2u, 3u, 2u, 2u},
        {2u, 2u, 3u, 4u, 2u, 2u},
    }));
    REQUIRE(bulk_postings.size() == source.runs.size());
    REQUIRE(bulk_span_strands.size() == source.runs.size());
    for (std::size_t ordinal = 0u;
         ordinal < source.runs.size();
         ++ordinal) {
        CHECK(bulk_postings[ordinal]
            == source.runs[ordinal].encoded_positions);
    }
    CHECK(bulk_span_strands[0] == std::vector<bool>{false});
    CHECK(bulk_span_strands[1]
        == (std::vector<bool>{false, true}));
    CHECK(bulk_span_strands[2]
        == (std::vector<bool>{false, true}));

    const auto range_accessor =
        idx::bind_compact_hash32_direct_index_selection_spans(
            view, *equal);
    REQUIRE(range_accessor.has_value());
    CHECK(range_accessor->selection_count() == equal->selections.size());
    std::vector<std::tuple<std::uint32_t, std::uint32_t,
        std::uint32_t, std::uint32_t, std::uint32_t, std::uint8_t>>
        ranged_callback_order;
    std::vector<std::vector<std::uint64_t>> ranged_postings(
        equal->selections.size());
    const auto visit_range = [&](std::uint64_t selection_begin,
                                 std::uint64_t selection_end) {
        return idx::visit_compact_hash32_direct_index_selection_span_range(
            *range_accessor,
            selection_begin,
            selection_end,
            [&](const idx::compact_hash32_selection_posting_spans& selected) {
                ranged_callback_order.emplace_back(
                    selected.selection_ordinal,
                    selected.dense_ordinal,
                    selected.query_ordinal,
                    selected.global_count,
                    selected.strand_zero_count,
                    selected.span_count);
                if (selected.selection_ordinal
                        >= ranged_postings.size()) {
                    return false;
                }
                for (std::uint8_t span_ordinal = 0u;
                     span_ordinal < selected.span_count;
                     ++span_ordinal) {
                    const auto& span = selected.spans[span_ordinal];
                    const auto decoded = span.visit_encoded_positions(
                        [&](std::uint64_t encoded) {
                            ranged_postings[selected.selection_ordinal]
                                .push_back(encoded);
                            return true;
                        });
                    if (!decoded || *decoded != span.count) return false;
                }
                return true;
            });
    };
    const auto first_shard = visit_range(0u, 2u);
    const auto second_shard = visit_range(2u, 3u);
    REQUIRE(first_shard.has_value());
    REQUIRE(second_shard.has_value());
    CHECK(ranged_callback_order == callback_order);
    CHECK(ranged_postings == bulk_postings);
    CHECK(first_shard->selection_callbacks
            + second_shard->selection_callbacks
        == bulk_visited->selection_callbacks);
    CHECK(first_shard->spans_exposed + second_shard->spans_exposed
        == bulk_visited->spans_exposed);
    CHECK(first_shard->postings_exposed
            + second_shard->postings_exposed
        == bulk_visited->postings_exposed);
    CHECK(first_shard->inline_selection_callbacks
            + second_shard->inline_selection_callbacks
        == bulk_visited->inline_selection_callbacks);
    CHECK(first_shard->materialized_selection_callbacks
            + second_shard->materialized_selection_callbacks
        == bulk_visited->materialized_selection_callbacks);
    CHECK(first_shard->inline_postings_exposed
            + second_shard->inline_postings_exposed
        == bulk_visited->inline_postings_exposed);
    CHECK(first_shard->materialized_postings_exposed
            + second_shard->materialized_postings_exposed
        == bulk_visited->materialized_postings_exposed);
    CHECK(first_shard->descriptors_examined
            + second_shard->descriptors_examined
        == bulk_visited->descriptors_examined);

    // The authenticated accessor is intentionally reusable: a second pass
    // over the same shard boundaries does not rescan the whole plan seal.
    std::uint64_t second_pass_callbacks = 0u;
    for (const auto [selection_begin, selection_end] :
         std::array{
             std::pair{std::uint64_t{0u}, std::uint64_t{2u}},
             std::pair{std::uint64_t{2u}, std::uint64_t{3u}},
         }) {
        const auto revisited =
            idx::visit_compact_hash32_direct_index_selection_span_range(
                *range_accessor,
                selection_begin,
                selection_end,
                [&](const idx::compact_hash32_selection_posting_spans&) {
                    ++second_pass_callbacks;
                    return true;
                });
        REQUIRE(revisited.has_value());
    }
    CHECK(second_pass_callbacks == equal->selections.size());

    for (const std::uint64_t empty_boundary :
         std::array<std::uint64_t, 3u>{0u, 1u, 3u}) {
        std::uint64_t empty_callbacks = 0u;
        const auto empty =
            idx::visit_compact_hash32_direct_index_selection_span_range(
                *range_accessor,
                empty_boundary,
                empty_boundary,
                [&](const idx::compact_hash32_selection_posting_spans&) {
                    ++empty_callbacks;
                    return true;
                });
        REQUIRE(empty.has_value());
        CHECK(empty_callbacks == 0u);
        CHECK(empty->selection_callbacks == 0u);
        CHECK(empty->spans_exposed == 0u);
        CHECK(empty->postings_exposed == 0u);
        CHECK(empty->descriptors_examined == 0u);
    }
    for (const auto [selection_begin, selection_end] :
         std::array{
             std::pair{std::uint64_t{2u}, std::uint64_t{1u}},
             std::pair{std::uint64_t{0u}, std::uint64_t{4u}},
             std::pair{std::uint64_t{4u}, std::uint64_t{4u}},
         }) {
        std::uint64_t invalid_callbacks = 0u;
        const auto invalid =
            idx::visit_compact_hash32_direct_index_selection_span_range(
                *range_accessor,
                selection_begin,
                selection_end,
                [&](const idx::compact_hash32_selection_posting_spans&) {
                    ++invalid_callbacks;
                    return true;
                });
        REQUIRE_FALSE(invalid.has_value());
        CHECK(invalid.error()
            == idx::direct_inverted_index_error::query_plan);
        CHECK(invalid_callbacks == 0u);
    }

    auto descriptor_mismatched_view = view;
    --descriptor_mismatched_view.header.descriptors_bytes;
    const auto descriptor_mismatched =
        idx::bind_compact_hash32_direct_index_selection_spans(
            descriptor_mismatched_view, *equal);
    REQUIRE_FALSE(descriptor_mismatched.has_value());
    CHECK(descriptor_mismatched.error()
        == idx::direct_inverted_index_error::query_plan);

    const auto require_unique_plan_rejected_without_callback =
        [&view](const idx::compact_hash32_unique_query_plan& forged) {
            const auto binding =
                idx::bind_compact_hash32_direct_index_selection_spans(
                    view, forged);
            REQUIRE_FALSE(binding.has_value());
            CHECK(binding.error()
                == idx::direct_inverted_index_error::query_plan);
            std::uint64_t callback_count = 0u;
            const auto rejected =
                idx::visit_compact_hash32_direct_index_selection_spans(
                    view,
                    forged,
                    [&](const idx::compact_hash32_selection_posting_spans&) {
                        ++callback_count;
                        return true;
                    });
            REQUIRE_FALSE(rejected.has_value());
            CHECK(rejected.error()
                == idx::direct_inverted_index_error::query_plan);
            CHECK(callback_count == 0u);
        };

    auto forged_valid_dense = *equal;
    forged_valid_dense.selections.front().dense_ordinal = 1u;
    require_unique_plan_rejected_without_callback(forged_valid_dense);

    auto forged_out_of_range_dense = *equal;
    forged_out_of_range_dense.selections.front().dense_ordinal =
        static_cast<std::uint32_t>(view.header.accepted_hash_count);
    require_unique_plan_rejected_without_callback(
        forged_out_of_range_dense);

    idx::compact_hash32_unique_query_plan unsealed{};
    unsealed.image_header_crc64 = equal->image_header_crc64;
    unsealed.max_occ_cap = equal->max_occ_cap;
    unsealed.runtime_max_occ = equal->runtime_max_occ;
    unsealed.posting_width_bytes = equal->posting_width_bytes;
    unsealed.selections = equal->selections;
    unsealed.stats = equal->stats;
    require_unique_plan_rejected_without_callback(unsealed);

    const std::array<std::uint32_t, 3u> all_accepted_hashes{2u, 5u, 7u};
    const auto full = idx::query_compact_hash32_direct_index_unique(
        view, all_accepted_hashes, 4u);
    REQUIRE(full.has_value());
    CHECK(full->selections.size() == all_accepted_hashes.size());
    CHECK(full->selections[0].dense_ordinal == 0u);
    CHECK(full->selections[0].query_ordinal == 0u);
    CHECK(full->selections[1].dense_ordinal == 1u);
    CHECK(full->selections[1].query_ordinal == 1u);
    CHECK(full->selections[2].dense_ordinal == 2u);
    CHECK(full->selections[2].query_ordinal == 2u);
    CHECK(full->selections.capacity() == all_accepted_hashes.size());
    CHECK(full->stats.selection_reserve_requested_elements == 3u);
    CHECK(full->stats.selection_initial_capacity_elements == 3u);
    CHECK(full->stats.selection_fill_reallocation_count == 0u);
    CHECK(full->stats.selection_compaction_allocation_count == 0u);
    CHECK(full->stats.selection_storage_allocation_count == 1u);
    CHECK(full->stats.selection_peak_capacity_bytes
        == 3u * sizeof(idx::compact_hash32_unique_selection));
    CHECK(full->stats.selection_retained_capacity_elements == 3u);
    CHECK(full->stats.selection_retained_capacity_bytes
        == 3u * sizeof(idx::compact_hash32_unique_selection));

    const std::array<std::uint32_t, 4u> all_absent_hashes{1u, 3u, 4u, 6u};
    const auto none = idx::query_compact_hash32_direct_index_unique(
        view, all_absent_hashes, 4u);
    REQUIRE(none.has_value());
    CHECK(none->selections.empty());
    CHECK(none->selections.capacity() == 0u);
    CHECK(none->stats.selection_reserve_requested_elements == 4u);
    CHECK(none->stats.selection_initial_capacity_elements == 4u);
    CHECK(none->stats.selection_fill_reallocation_count == 0u);
    CHECK(none->stats.selection_compaction_allocation_count == 0u);
    CHECK(none->stats.selection_storage_allocation_count == 1u);
    CHECK(none->stats.selection_peak_capacity_bytes
        == 4u * sizeof(idx::compact_hash32_unique_selection));
    CHECK(none->stats.selection_retained_capacity_elements == 0u);
    CHECK(none->stats.selection_retained_capacity_bytes == 0u);
    std::uint64_t absent_selection_callbacks = 0u;
    const auto none_visited =
        idx::visit_compact_hash32_direct_index_selection_spans(
            view,
            *none,
            [&](const idx::compact_hash32_selection_posting_spans&) {
                ++absent_selection_callbacks;
                return true;
            });
    REQUIRE(none_visited.has_value());
    CHECK(absent_selection_callbacks == 0u);
    CHECK(none_visited->selection_callbacks == 0u);
    CHECK(none_visited->spans_exposed == 0u);
    CHECK(none_visited->postings_exposed == 0u);
    CHECK(none_visited->inline_selection_callbacks == 0u);
    CHECK(none_visited->materialized_selection_callbacks == 0u);
    CHECK(none_visited->inline_postings_exposed == 0u);
    CHECK(none_visited->materialized_postings_exposed == 0u);
    CHECK(none_visited->descriptors_examined == 0u);
    CHECK(none_visited->rank_checkpoints_examined == 0u);
    CHECK(none_visited->membership_words_examined == 0u);
    const auto none_range_accessor =
        idx::bind_compact_hash32_direct_index_selection_spans(
            view, *none);
    REQUIRE(none_range_accessor.has_value());
    CHECK(none_range_accessor->selection_count() == 0u);
    const auto none_range =
        idx::visit_compact_hash32_direct_index_selection_span_range(
            *none_range_accessor,
            0u,
            0u,
            [](const idx::compact_hash32_selection_posting_spans&) {
                return false;
            });
    REQUIRE(none_range.has_value());
    CHECK(none_range->selection_callbacks == 0u);
    CHECK(none_range->postings_exposed == 0u);

    const auto greater = idx::query_compact_hash32_direct_index_unique(
        view, unique_hashes, 5u);
    REQUIRE_FALSE(greater.has_value());
    CHECK(greater.error()
        == idx::direct_inverted_index_error::storage_contract_mismatch);
    const auto zero = idx::query_compact_hash32_direct_index_unique(
        view, unique_hashes, 0u);
    REQUIRE_FALSE(zero.has_value());
    CHECK(zero.error()
        == idx::direct_inverted_index_error::storage_contract_mismatch);

    const std::array<std::uint32_t, 2u> duplicate_hashes{2u, 2u};
    const auto duplicate = idx::query_compact_hash32_direct_index_unique(
        view, duplicate_hashes, 2u);
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error() == idx::direct_inverted_index_error::order);
    const auto duplicate_projected =
        idx::query_compact_hash32_direct_index_unique_projected(
            view,
            duplicate_hashes.size(),
            [&duplicate_hashes](std::size_t ordinal) noexcept
                    -> std::uint32_t {
                return duplicate_hashes[ordinal];
            },
            2u);
    REQUIRE_FALSE(duplicate_projected.has_value());
    CHECK(duplicate_projected.error()
        == idx::direct_inverted_index_error::order);
}

TEST_CASE(
    "compact exact-domain hash32 visitor rejects unbound misaligned and "
    "out-of-section spans",
    "[component:bioseq][layer:unit][feature:index]") {
    compact_test_source source{
        .runs = {
            compact_test_run{
                .header = {.hash = 10u, .global_count = 2u,
                    .target_strand_zero_count = 2u},
                .encoded_positions = {
                    encoded_position(1u, false),
                    encoded_position(3u, false),
                },
            },
        },
    };
    const auto build_options = compact_options(100u, 2u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const auto view = validate_compact_or_fail(image, build_options);
    const std::array query{
        idx::compact_hash32_query_occurrence{
            .hash = 10u, .query_strand_one = false},
    };
    const auto planned = idx::query_compact_hash32_direct_index(
        view, query, 2u);
    REQUIRE(planned.has_value());
    REQUIRE(planned->selections.size() == 1u);
    REQUIRE(planned->spans.size() == 1u);

    const auto require_plan_rejected = [&view](
            const idx::compact_hash32_query_plan& forged) {
        collected_postings collected{
            .by_selection = std::vector<std::vector<std::uint64_t>>(1u),
        };
        const auto visited = idx::visit_compact_hash32_direct_index_postings(
            view, forged, &collected, &collect_posting);
        REQUIRE_FALSE(visited.has_value());
        CHECK(visited.error() == idx::direct_inverted_index_error::query_plan);
        CHECK(collected.callback_count == 0u);
    };

    auto misaligned = *planned;
    ++misaligned.spans.front().byte_offset;
    require_plan_rejected(misaligned);

    auto out_of_section = *planned;
    out_of_section.spans.front().byte_offset =
        view.header.postings_offset + view.header.postings_bytes;
    require_plan_rejected(out_of_section);

    auto unbound = *planned;
    ++unbound.selections.front().hash;
    require_plan_rejected(unbound);
}
