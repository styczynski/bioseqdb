/**
 * @file
 * @brief Check packed-sequence conversion and selection by alphabet and storage width.
 */

#include <cstring>
#include <hwy/highway.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.common.templating;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

namespace {

using test_supported_alphabets =
    supported_alphabets<alphabet_entry<seq::dna4, "dna4">, alphabet_entry<seq::dna5, "dna5">>;
using test_stdc_t = compact_sequence_stdc<test_supported_alphabets>;

/**
 * @brief Keep factory-allocated test buffers alive until the arena is destroyed.
 *
 * Each allocation returns a separate zeroed block.
 */
struct test_arena {
    std::vector<std::unique_ptr<std::byte[]>> blocks;

    /** Return a factory allocator that keeps its zeroed buffers in this arena. */
    [[nodiscard]] auto alloc_fn() {
        return [this](std::size_t byte_count) -> test_stdc_t* {
            auto buffer = std::make_unique<std::byte[]>(byte_count);
            std::memset(buffer.get(), 0, byte_count);
            auto* header = reinterpret_cast<test_stdc_t*>(buffer.get());
            // Initialize the byte count before the factory fills the header.
            header->vl_len_ = static_cast<test_stdc_t::varlena_header_t>(byte_count);
            blocks.push_back(std::move(buffer));
            return header;
        };
    }
};

} // anonymous namespace

TEST_CASE("compact sequence conversion preserves ASCII text",
          "[component:bioseq][layer:unit][feature:sequence]") {
    const std::string input = "ACGTACGTACGTACGTACGTACGTACGTACGT";

    SECTION("dna4 across {1,2,4,8} byte widths") {
        for (std::uint8_t bw : {1u, 2u, 4u, 8u}) {
            test_arena arena;
            auto* hdr = stdc_convert_from_cstr<test_stdc_t, seq::dna4>(
                bw,
                static_cast<test_stdc_t::bp_size_t>(input.size()),
                input.data(),
                arena.alloc_fn());
            REQUIRE(hdr != nullptr);
            REQUIRE(static_cast<std::size_t>(hdr->bp_size) == input.size());
            REQUIRE(hdr->storage_unit_byte_width == bw);
            REQUIRE(hdr->alphabet_code == 0u); // dna4 is first in the test alphabet list.

            std::string decoded;
            hdr->convert_to(
                [&decoded]<class p_sequence>(const compact_sequence_range<p_sequence>& seq) {
                    decoded = ::bioseq::simd::active_target::decode_sequence_simd<p_sequence>(seq);
                });
            REQUIRE(decoded == input);
        }
    }

    SECTION("dna5 across {1,2,4,8} byte widths") {
        const std::string with_n = "ACGTNNACGTACGTNNNACGTACGTACGTACGT";
        for (std::uint8_t bw : {1u, 2u, 4u, 8u}) {
            test_arena arena;
            auto* hdr = stdc_convert_from_cstr<test_stdc_t, seq::dna5>(
                bw,
                static_cast<test_stdc_t::bp_size_t>(with_n.size()),
                with_n.data(),
                arena.alloc_fn());
            REQUIRE(hdr != nullptr);
            REQUIRE(hdr->alphabet_code == 1u);
            std::string decoded;
            hdr->convert_to(
                [&decoded]<class p_sequence>(const compact_sequence_range<p_sequence>& seq) {
                    decoded = ::bioseq::simd::active_target::decode_sequence_simd<p_sequence>(seq);
                });
            REQUIRE(decoded == with_n);
        }
    }
}

TEST_CASE("dna4 uint64 multiline SIMD encoding stops at sentinel chunks",
          "[component:bioseq][layer:unit][feature:sequence][resource:highway]") {
    using config_t = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using range_t = compact_sequence_range<config_t>;

    static constexpr char bases[] = "ACGT";
    std::string body;
    body.reserve(997);
    for (std::size_t i = 0; i < 997; ++i) {
        body.push_back(bases[((i * 5u) + (i / 7u) + 3u) & 3u]);
    }

    std::string wrapped;
    for (std::size_t i = 0; i < body.size(); i += 60) {
        const std::size_t n = std::min<std::size_t>(60, body.size() - i);
        wrapped.append(body.data() + i, n);
        wrapped.push_back('\n');
    }
    const std::string input = wrapped + ">next\nACGT\n";

    std::vector<std::uint64_t> packed;
    packed.reserve((body.size() + 31u) / 32u);
    encode_state<std::uint64_t, 2, 32> state{};
    state.output_storage = &packed;
    state.reset();

    std::size_t newline_count = 0;
    const std::size_t consumed =
        encode_body_chunk_dna4_simd_multiline_until_byte<std::uint8_t{'>'}, std::uint64_t, 2, 32>(
            state,
            reinterpret_cast<const std::uint8_t*>(input.data()),
            input.size(),
            &newline_count);
    REQUIRE(consumed == wrapped.size());
    REQUIRE(newline_count == (body.size() + 59u) / 60u);

    const auto [n_words, padding] = finalize_encode_state<std::uint64_t, 2, 32>(state);
    REQUIRE(n_words == packed.size());

    auto seq = range_t::from_packed(std::move(packed), padding);
    REQUIRE(seq.bp_size() == body.size());
    REQUIRE(decode_sequence_simd<config_t>(seq) == body);
}

TEST_CASE("storage send and receive preserve packed bytes",
          "[component:bioseq][layer:unit][feature:sequence]") {
    const std::string input = "ACGTACGTACGTACGT";
    for (std::uint8_t bw : {1u, 2u, 4u, 8u}) {
        test_arena arena;
        auto* src = stdc_convert_from_cstr<test_stdc_t, seq::dna4>(
            bw, static_cast<test_stdc_t::bp_size_t>(input.size()), input.data(), arena.alloc_fn());
        REQUIRE(src != nullptr);

        // Save the units sent by the storage format for a receive round trip.
        std::vector<std::uint64_t> wire_units;
        wire_units.reserve(src->unit_count());
        src->send_to(wire_units,
                     []<class p_input>(std::vector<std::uint64_t>& buf,
                                       std::uint64_t unit,
                                       std::uint8_t /*byte_width*/) { buf.push_back(unit); });
        REQUIRE(wire_units.size() == src->unit_count());

        // Feed the saved units into convert_from in the same order.
        /// Track the next saved unit to read.
        struct cursor {
            std::size_t idx;
        };
        cursor cur{0};
        auto reader = [&wire_units]<class p_alphabet, class p_output>(
                          cursor& c, std::uint8_t /*byte_width*/) -> p_output {
            return static_cast<p_output>(wire_units[c.idx++]);
        };
        auto* dst =
            test_stdc_t::convert_from<seq::dna4>(bw,
                                                 src->padding_letters,
                                                 src->bp_size,
                                                 static_cast<std::uint32_t>(src->unit_count()),
                                                 cur,
                                                 reader,
                                                 arena.alloc_fn());
        REQUIRE(dst != nullptr);
        REQUIRE(dst->bp_size == src->bp_size);
        REQUIRE(dst->padding_letters == src->padding_letters);
        REQUIRE(dst->storage_unit_byte_width == src->storage_unit_byte_width);
        // The round trip must preserve every payload byte.
        REQUIRE(std::memcmp(src->data, dst->data, src->unit_count() * bw) == 0);
    }
}

TEST_CASE("same-alphabet storage-width recasting preserves packed bytes",
          "[component:bioseq][layer:unit][feature:sequence]") {
    const std::string input = "ACGTACGTACGTACGTACGTACGTACGT";
    test_arena arena;

    // Start with 64-bit packed words.
    auto* src_8 = stdc_convert_from_cstr<test_stdc_t, seq::dna4>(
        8u, static_cast<test_stdc_t::bp_size_t>(input.size()), input.data(), arena.alloc_fn());
    REQUIRE(src_8 != nullptr);

    // Convert to byte-sized units and back to check that repacking preserves data.
    auto* mid_1 = src_8->recast_storage_width(1u, arena.alloc_fn());
    REQUIRE(mid_1 != nullptr);
    REQUIRE(mid_1->alphabet_code == src_8->alphabet_code);
    REQUIRE(mid_1->bp_size == src_8->bp_size);

    auto* back_8 = mid_1->recast_storage_width(8u, arena.alloc_fn());
    REQUIRE(back_8 != nullptr);
    REQUIRE(std::memcmp(src_8->data, back_8->data, src_8->unit_count() * 8u) == 0);
}

TEST_CASE("zero-copy compact sequence views preserve decoded text",
          "[component:bioseq][layer:unit][feature:sequence]") {
    const std::string input = "ACGTACGTACGTACGT";
    for (std::uint8_t bw : {1u, 2u, 4u, 8u}) {
        test_arena arena;
        auto* hdr = stdc_convert_from_cstr<test_stdc_t, seq::dna4>(
            bw, static_cast<test_stdc_t::bp_size_t>(input.size()), input.data(), arena.alloc_fn());
        REQUIRE(hdr != nullptr);

        // Decode the selected view and compare it with the original text.
        std::string view_str;
        hdr->convert_to(
            [&view_str]<class p_sequence>(const compact_sequence_range<p_sequence>& seq) {
                view_str = seq.to_string();
            });
        REQUIRE(view_str == input);
    }
}

TEST_CASE("compact sequence type parameters round-trip through encoding",
          "[component:bioseq][layer:unit][feature:sequence]") {
    for (std::uint8_t alpha : {0u, 1u}) {
        for (std::uint8_t bw : {1u, 2u, 4u, 8u}) {
            const compact_sequence_stdc_type_params t{
                .alphabet_code = alpha,
                .storage_unit_byte_width = bw,
            };
            const auto encoded = t.encode();
            const auto decoded = compact_sequence_stdc_type_params::decode(encoded);
            REQUIRE(decoded == t);
        }
    }
}

TEST_CASE("compact sequence data offsets remain uint64-aligned across pack widths",
          "[component:bioseq][layer:unit][feature:sequence]") {
    using supported_test_alphabets =
        supported_alphabets<alphabet_entry<seq::dna4, "dna4">, alphabet_entry<seq::dna5, "dna5">>;
    using stdc_default = compact_sequence_stdc<supported_test_alphabets>;
    using stdc_bp16 = compact_sequence_stdc<supported_test_alphabets, std::uint16_t>;
    using stdc_bp8 = compact_sequence_stdc<supported_test_alphabets, std::uint8_t>;
    using stdc_bp64 = compact_sequence_stdc<supported_test_alphabets, std::uint64_t>;
    REQUIRE(offsetof(stdc_default, data) % 8u == 0u);
    REQUIRE(offsetof(stdc_bp16, data) % 8u == 0u);
    REQUIRE(offsetof(stdc_bp8, data) % 8u == 0u);
    REQUIRE(offsetof(stdc_bp64, data) % 8u == 0u);
}

TEST_CASE("compile-time strings support composition and comparison",
          "[component:bioseq][layer:unit][feature:alphabet]") {
    using namespace ::bioseq::common::templating;
    constexpr compile_time_string a{"foo"};
    constexpr compile_time_string b{"bar"};
    STATIC_REQUIRE(a.size() == 3);
    STATIC_REQUIRE(a == "foo");
    STATIC_REQUIRE((a + b) == "foobar");
    STATIC_REQUIRE(a.equals_c_string("foo"));
    STATIC_REQUIRE(!a.equals_c_string("food"));
    STATIC_REQUIRE(!a.equals_c_string("fo"));
}

TEST_CASE("supported alphabet names and codes round-trip",
          "[component:bioseq][layer:unit][feature:alphabet]") {
    using sa = test_supported_alphabets;
    STATIC_REQUIRE(sa::name_of<seq::dna4>() == "dna4");
    STATIC_REQUIRE(sa::name_of<seq::dna5>() == "dna5");
    REQUIRE(sa::name_of_alphabet_code(sa::index_of<seq::dna4>()) == std::string_view{"dna4"});
    REQUIRE(sa::name_of_alphabet_code(sa::index_of<seq::dna5>()) == std::string_view{"dna5"});

    REQUIRE(sa::try_index_of_name("dna4") == std::optional<sa::alphabet_code_t>{0});
    REQUIRE(sa::try_index_of_name("dna5") == std::optional<sa::alphabet_code_t>{1});
    REQUIRE(sa::try_index_of_name("xyz") == std::nullopt);
    REQUIRE(sa::try_index_of_name("dna") == std::nullopt);
    REQUIRE(sa::try_index_of_name("dna44") == std::nullopt);
    REQUIRE(sa::try_index_of_name("") == std::nullopt);
}

TEST_CASE("supported alphabet names produce the validation list",
          "[component:bioseq][layer:unit][feature:alphabet]") {
    REQUIRE(test_supported_alphabets::valid_alphabet_names_csv() ==
            std::string_view{"'dna4', 'dna5'"});
}

TEST_CASE("supported alphabet packs enforce unique types and names",
          "[component:bioseq][layer:unit][feature:alphabet]") {
    STATIC_REQUIRE(test_supported_alphabets::alphabet_count == 2u);
    using sa = test_supported_alphabets;
    STATIC_REQUIRE(std::is_same_v<sa::entry_for<seq::dna4>::alphabet_type, seq::dna4>);
    STATIC_REQUIRE(std::is_same_v<sa::entry_for<seq::dna5>::alphabet_type, seq::dna5>);
    STATIC_REQUIRE(sa::entry_for<seq::dna4>::alphabet_name_sv == "dna4");

    // Check the type and name uniqueness rules directly.
    STATIC_REQUIRE(
        ::bioseq::common::templating::unique_types_in_tuple_v<typename sa::alphabets_tuple>);
    STATIC_REQUIRE(dispatch_detail::unique_names_in_pack<alphabet_entry<seq::dna4, "dna4">,
                                                         alphabet_entry<seq::dna5, "dna5">>());
}

TEST_CASE("alphabet name dispatch selects the matching entry type",
          "[component:bioseq][layer:unit][feature:alphabet]") {
    bool saw_dna4 = false;
    const bool matched_dna4 =
        test_supported_alphabets::dispatch_alphabet_by_name("dna4", [&saw_dna4]<class p_entry>() {
            saw_dna4 = std::is_same_v<typename p_entry::alphabet_type, seq::dna4>;
        });
    REQUIRE(matched_dna4);
    REQUIRE(saw_dna4);

    bool saw_unknown = true;
    const bool matched_unknown = test_supported_alphabets::dispatch_alphabet_by_name(
        "rna7", [&saw_unknown]<class /*p_entry*/>() { saw_unknown = false; });
    REQUIRE_FALSE(matched_unknown);
    REQUIRE(saw_unknown); // An unknown name must leave the callback uncalled.
}

}; // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();
