#include <catch2/catch_test_macros.hpp>
import std;
import bioseq.common.seqan_adapter;
import bioseq.parallelism.memory_admission;
import bioseq.sequences;
import bioseq.sequences.quality_summary;
import bioseq.sequences.translation;
import bioseq.sequences.simd;
import bioseq.storage.lazy_sequence;
import bioseq.indexing.symbol_counts;
import bioseq.tests.support.indexing_storage_fakes;
using namespace bioseq::sequences;

TEST_CASE("quality exact arithmetic spans negative and near bigint limits", "[component:bioseq][layer:unit][feature:quality]") {
    const std::array<std::uint8_t, 3> ranks{0, 5, 67};
    const auto value = summarize_quality_ranks<seq::phred68solexa>(ranks);
    REQUIRE(value.has_value());
    REQUIRE(value->length == 3);
    REQUIRE(value->sum == 57);
    REQUIRE(value->minimum == -5);
    REQUIRE(value->maximum == 62);
    REQUIRE(quality_mean_decimal(*value) == "19");
    REQUIRE(quality_mean_decimal({3, -1, -1, 0}) == "-0.3333333333333333333333333333333333333333");
    REQUIRE(quality_mean_decimal({6, 1, 0, 1}) == "0.1666666666666666666666666666666666666667");
    REQUIRE(quality_mean_decimal({std::uint64_t{1} << 41, 1, 0, 1}) ==
        "0.0000000000004547473508864641189575195313");
    REQUIRE(quality_mean_decimal({std::uint64_t{1} << 41, -1, -1, 0}) ==
        "-0.0000000000004547473508864641189575195313");
    REQUIRE(quality_mean_decimal({}).has_value() == false);
    const auto n = quality_max_length;
    const quality_summary huge{n, quality_sum{62} * n, 62, 62};
    REQUIRE(valid_quality_summary<seq::phred63>(huge));
    REQUIRE(quality_sum_decimal(huge.sum) == "571849066284996100034");
    REQUIRE(quality_mean_decimal(huge) == "62");
    REQUIRE_FALSE(merge_quality_summaries<seq::phred63>(huge, {1, 1, 1, 1}));
    REQUIRE(merge_quality_summaries<seq::phred63>(huge, {})->sum == huge.sum);
    const auto a = make_quality_mean_key({n, quality_sum{30} * n + 1, 30, 31});
    const auto b = make_quality_mean_key({n - 1, quality_sum{30} * (n - 1) + 1, 30, 31});
    REQUIRE(compare_quality_means(a, b) < 0);
    REQUIRE(quality_mean_decimal(a.summary) != quality_mean_decimal(b.summary));
    REQUIRE(compare_quality_means(make_quality_mean_key({3,-1,-1,0}), make_quality_mean_key({2,-1,-1,0})) > 0);
    REQUIRE(compare_quality_means(make_quality_mean_key({3,-1,-1,0}), make_quality_mean_key({6,-2,-1,0})) == 0);
    const quality_summary low{3,-15,-5,-5}, high{2,10,5,5}, neutral{1,5,5,5};
    const auto merged = merge_quality_summaries<seq::phred68solexa>(low, high);
    REQUIRE(merged.has_value());
    const auto total = merge_quality_summaries<seq::phred68solexa>(*merged, neutral);
    REQUIRE(total.has_value()); REQUIRE(total->length == 6); REQUIRE(total->sum == 0);
    REQUIRE(quality_mean_decimal(*total) == "0");
    const auto right = merge_quality_summaries<seq::phred68solexa>(high, neutral);
    REQUIRE(right.has_value());
    const auto associated = merge_quality_summaries<seq::phred68solexa>(low, *right);
    REQUIRE(associated.has_value());
    REQUIRE(*total == *associated);
}

template<class Alphabet> void check_quality_wire() {
    quality_index_payload<Alphabet> value{};
    value.source_epoch = 123;
    value.counts[0] = 2;
    value.counts[seqan3::alphabet_size<Alphabet>-1] = 3;
    value.summary = *summarize_quality_histogram<Alphabet>(value.counts);
    const auto encoded = encode_quality_index(value);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded->size() == 64 + 8 * quality_histogram_slots<Alphabet>);
    const auto read = decode_quality_index<Alphabet>(*encoded, 123, 5);
    REQUIRE(read.has_value());
    REQUIRE(read->summary == value.summary);
    REQUIRE(read->counts == value.counts);
    REQUIRE(decode_quality_index<Alphabet>(*encoded, 124, 5).error() == quality_error::stale_source);
    for (const auto offset : {0, 6, 8, 9, 10, 11, 12, 14, 16, 24, 32, 40, 42, 44, 56, 64}) {
        auto broken = *encoded;
        broken[offset] ^= std::byte{128};
        REQUIRE_FALSE(decode_quality_index<Alphabet>(broken, 123, 5));
    }
    auto newer = *encoded;
    newer[4] = std::byte{2};
    REQUIRE(decode_quality_index<Alphabet>(newer, 123, 5).error() == quality_error::unsupported_version);
    REQUIRE_FALSE(decode_quality_index<Alphabet>(std::span{encoded->data(), encoded->size()-1}, 123, 5));
    value.counts[seqan3::alphabet_size<Alphabet>] = 1;
    REQUIRE_FALSE(encode_quality_index(value));
}
TEST_CASE("quality payload roundtrips and rejects corrupt headers and histograms", "[component:bioseq][layer:unit][feature:quality]") {
    check_quality_wire<seq::phred42>();
    check_quality_wire<seq::phred63>();
    check_quality_wire<seq::phred68solexa>();
    quality_index_payload<seq::phred68solexa> negative{};
    negative.source_epoch = 1;
    negative.counts[0] = 5;
    negative.summary = *summarize_quality_histogram<seq::phred68solexa>(negative.counts);
    const auto wire = encode_quality_index(negative);
    REQUIRE(wire.has_value());
    REQUIRE((*wire)[24] == std::byte{0xe7}); // -25, little-endian two's complement.
    for (std::size_t offset = 25; offset < 40; ++offset) REQUIRE((*wire)[offset] == std::byte{0xff});
    REQUIRE(decode_quality_index<seq::phred68solexa>(*wire, 1, 5)->summary == negative.summary);
}

template<class Alphabet> void check_quality_index() {
    using config = sequence_custom_alphabet<Alphabet, std::uint64_t>;
    std::vector<Alphabet> letters;
    for (std::size_t i = 0; i < 4099; ++i)
        letters.push_back(seqan3::assign_rank_to(i % seqan3::alphabet_size<Alphabet>, Alphabet{}));
    compact_sequence_range<config> range{letters};
    const auto owner = bioseq::storage::make_resident_lazy_sequence(range);
    const auto view = bioseq::storage::make_lazy_sequence_view(owner);
    struct checked_storage : bioseq::indexing::test::fake_sequence_storage {
        [[noreturn]] void index_error(std::string_view message) const {
            throw std::runtime_error(std::string{message});
        }
    } store;
    using index = bioseq::indexing::symbol_counts_index<decltype(store)>;
    index::build(store, view, 55);
    const auto read = index::template read_summary<Alphabet>(store, 55, letters.size());
    REQUIRE(read.has_value()); REQUIRE(read->has_value());
    const auto expected = summarize_quality_source<Alphabet>(view.template borrow_rank_source<false>(), 0, letters.size());
    REQUIRE(expected.has_value()); REQUIRE(**read == *expected);
    REQUIRE_FALSE(*index::template read_summary<Alphabet>(store, 56, letters.size()));
    REQUIRE_THROWS(index::read_count(store, 0));
    index::rebuild(store, view, 56);
    REQUIRE_FALSE(*index::template read_summary<Alphabet>(store, 55, letters.size()));
    REQUIRE(**index::template read_summary<Alphabet>(store, 56, letters.size()) == *expected);
}
TEST_CASE("quality symbol_counts uses the existing storage and source reader", "[component:bioseq][layer:unit][feature:quality]") {
    check_quality_index<seq::phred42>(); check_quality_index<seq::phred63>(); check_quality_index<seq::phred68solexa>();
}

TEST_CASE("quality DAG repeated doubling visits each source once", "[component:bioseq][layer:unit][feature:quality]") {
    struct node { const node* left{}; const node* right{}; };
    std::array<node, 61> tree{};
    for (std::size_t i = 1; i < tree.size(); ++i) tree[i] = {&tree[i-1], &tree[i-1]};
    std::size_t reads = 0;
    const auto value = summarize_quality_dag<seq::phred42>(static_cast<const node*>(&tree.back()),
        [](const node* value) { return value; },
        [](const node* value) -> std::optional<std::pair<const node*, const node*>> {
            if (!value->left) return std::nullopt;
            return std::pair{value->left, value->right};
        },
        [&](const node*) -> std::expected<quality_summary, quality_error> { ++reads; return quality_summary{1,41,41,41}; });
    REQUIRE(value.has_value()); REQUIRE(reads == 1);
    REQUIRE(value->length == (std::uint64_t{1} << 60));
    REQUIRE(quality_mean_decimal(*value) == "41");
}

TEST_CASE("quality DAG admission failure releases reservations before retry",
          "[component:bioseq][layer:unit][feature:quality]") {
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    struct restore_cap {
        bioseq::parallelism::memory_admission_controller& controller;
        std::uint64_t previous;
        ~restore_cap() { controller.set_index_memory_cap_bytes(previous); }
    } restore{admission, admission.index_memory_cap_bytes()};
    const auto reserved = admission.reserved_bytes();
    int root{};
    std::size_t reads{};
    const auto reduce = [&] {
        return summarize_quality_dag<seq::phred42>(&root,
            [](const int* node) { return node; },
            [](const int*) -> std::optional<std::pair<int*, int*>> { return std::nullopt; },
            [&](const int*) -> std::expected<quality_summary, quality_error> {
                ++reads;
                return quality_summary{1, 41, 41, 41};
            });
    };
    admission.set_index_memory_cap_bytes(4095);
    const auto refused = reduce();
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error() == quality_error::memory_limit);
    REQUIRE(reads == 0);
    REQUIRE(admission.reserved_bytes() == reserved);
    admission.set_index_memory_cap_bytes(restore.previous);
    REQUIRE(reduce().has_value());
    REQUIRE(reads == 1);
    REQUIRE(admission.reserved_bytes() == reserved);
}

TEST_CASE("quality native conversion checks empty families and first narrowing failure",
          "[component:bioseq][layer:unit][feature:quality]") {
    using modern = sequence_custom_alphabet<seq::phred42, std::uint64_t>;
    using wider = sequence_custom_alphabet<seq::phred63, std::uint64_t>;
    using legacy = sequence_custom_alphabet<seq::phred68solexa, std::uint64_t>;
    compact_sequence_range<modern> empty{std::vector<seq::phred42>{}};
    const auto empty_source = bioseq::storage::make_borrowed_rank_source(empty);
    std::span<std::uint64_t> no_output;
    const auto incompatible = convert_packed_into<seq::phred42, legacy>(
        empty_source, 0, 0, {}, no_output, false, nullptr);
    REQUIRE_FALSE(incompatible);
    REQUIRE(incompatible.error().code == sequence_conversion_error_code::incompatible_alphabets);
    const auto empty_widened = convert_packed_into<seq::phred42, wider>(
        empty_source, 0, 0, {}, no_output, false, nullptr);
    REQUIRE(empty_widened.has_value());
    std::vector<seq::phred63> letters;
    for (const unsigned rank : {0u, 41u, 42u})
        letters.push_back(seqan3::assign_rank_to(rank, seq::phred63{}));
    compact_sequence_range<wider> source{letters};
    std::array<std::uint64_t, 1> output{};
    const auto narrowed = convert_packed_into<seq::phred63, modern>(
        bioseq::storage::make_borrowed_rank_source(source), 0, 3, {}, output, false, nullptr);
    REQUIRE_FALSE(narrowed);
    REQUIRE(narrowed.error().input_position == 2);
}
