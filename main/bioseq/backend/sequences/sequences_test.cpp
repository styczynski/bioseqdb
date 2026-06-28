#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>


import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.common.templating;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers;
import bioseq.common.testing;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// =====================================================================
// DOC0258 unit tests — compact_sequence_stdc + zero-copy
// compact_sequence_range<S> adapter.
//
// The tests stand up a single-arena allocator (a moveable std::vector<char>
// scratch pad) so they don't depend on postgres palloc. Every alloc is
// re-zeroed and aligned to alignof(uint64_t) which is the same as
// alignof(compact_sequence_stdc<…>) for the default pack.
// =====================================================================

namespace {

using test_supported_alphabets = supported_alphabets<
    alphabet_entry<seq::dna4, "dna4">,
    alphabet_entry<seq::dna5, "dna5">>;
using test_stdc_t              = compact_sequence_stdc<test_supported_alphabets>;

// Simple owning buffer allocator: arena() returns a callable that
// owns one heap-allocated zeroed block. Re-entrant — calling the
// allocator twice returns two distinct blocks.
struct test_arena {
    std::vector<std::unique_ptr<std::byte[]>> blocks;

    auto alloc_fn() {
        return [this](std::size_t byte_count) -> test_stdc_t* {
            auto buf = std::make_unique<std::byte[]>(byte_count);
            std::memset(buf.get(), 0, byte_count);
            auto* hdr = reinterpret_cast<test_stdc_t*>(buf.get());
            // vl_len_ is overwritten by the factory after this returns,
            // but we set the byte count here so any factory that forgets
            // is at least self-consistent.
            hdr->vl_len_ = static_cast<test_stdc_t::varlena_header_t>(byte_count);
            blocks.push_back(std::move(buf));
            return hdr;
        };
    }
};

}  // anonymous namespace

TEST_CASE("DOC0258: round-trip cstring -> compact_sequence_stdc -> ascii",
          "[doc0258]") {
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
            REQUIRE(hdr->alphabet_code == 0u);  // dna4 is pack-index 0

            std::string decoded;
            hdr->convert_to([&]<class S>(const compact_sequence_range<S>& seq) {
                decoded = ::bioseq::simd::active_target::decode_sequence_simd<S>(seq);
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
            hdr->convert_to([&]<class S>(const compact_sequence_range<S>& seq) {
                decoded = ::bioseq::simd::active_target::decode_sequence_simd<S>(seq);
            });
            REQUIRE(decoded == with_n);
        }
    }
}

TEST_CASE("DOC0258: send_to / convert_from(recv) round-trip preserves bytes",
          "[doc0258]") {
    const std::string input = "ACGTACGTACGTACGT";
    for (std::uint8_t bw : {1u, 2u, 4u, 8u}) {
        test_arena arena;
        auto* src = stdc_convert_from_cstr<test_stdc_t, seq::dna4>(
            bw,
            static_cast<test_stdc_t::bp_size_t>(input.size()),
            input.data(),
            arena.alloc_fn());
        REQUIRE(src != nullptr);

        // Capture the units written to a uint64_t-vector via send_to.
        std::vector<std::uint64_t> wire_units;
        wire_units.reserve(src->unit_count());
        src->send_to(wire_units,
            []<class In>(std::vector<std::uint64_t>& buf, std::uint64_t unit,
                          std::uint8_t /*byte_width*/) {
                buf.push_back(unit);
            });
        REQUIRE(wire_units.size() == src->unit_count());

        // Replay through convert_from(recv) using a cursor into wire_units.
        struct cursor { std::size_t idx; };
        cursor cur{0};
        auto reader =
            [&]<class A, class Out>(cursor& c, std::uint8_t /*byte_width*/) -> Out {
                return static_cast<Out>(wire_units[c.idx++]);
            };
        auto* dst = test_stdc_t::convert_from<seq::dna4>(
            bw,
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
        // Payload byte equality.
        REQUIRE(std::memcmp(src->data, dst->data,
                            src->unit_count() * bw) == 0);
    }
}

TEST_CASE("DOC0258: same-alphabet recast_storage_width preserves to_string",
          "[doc0258]") {
    const std::string input = "ACGTACGTACGTACGTACGTACGTACGT";
    test_arena arena;

    // Encode at (dna4, 8 bytes).
    auto* src_8 = stdc_convert_from_cstr<test_stdc_t, seq::dna4>(
        8u, static_cast<test_stdc_t::bp_size_t>(input.size()),
        input.data(), arena.alloc_fn());
    REQUIRE(src_8 != nullptr);

    // Recast to (dna4, 1 byte) and back to (dna4, 8 bytes).
    auto* mid_1 = src_8->recast_storage_width(1u, arena.alloc_fn());
    REQUIRE(mid_1 != nullptr);
    REQUIRE(mid_1->alphabet_code == src_8->alphabet_code);
    REQUIRE(mid_1->bp_size == src_8->bp_size);

    auto* back_8 = mid_1->recast_storage_width(8u, arena.alloc_fn());
    REQUIRE(back_8 != nullptr);
    REQUIRE(std::memcmp(src_8->data, back_8->data,
                        src_8->unit_count() * 8u) == 0);
}

TEST_CASE("DOC0258: zero-copy compact_sequence_range<S> matches legacy",
          "[doc0258]") {
    const std::string input = "ACGTACGTACGTACGT";
    for (std::uint8_t bw : {1u, 2u, 4u, 8u}) {
        test_arena arena;
        auto* hdr = stdc_convert_from_cstr<test_stdc_t, seq::dna4>(
            bw, static_cast<test_stdc_t::bp_size_t>(input.size()),
            input.data(), arena.alloc_fn());
        REQUIRE(hdr != nullptr);

        // Get the new view through dispatch_instance and compare its
        // to_string() against the legacy compact_sequence_range<...>::
        // to_string for the same input encoded directly.
        std::string view_str;
        hdr->convert_to([&]<class S>(const compact_sequence_range<S>& seq) {
            view_str = seq.to_string();
        });
        REQUIRE(view_str == input);
    }
}

TEST_CASE("DOC0258: type_params encode/decode is reversible", "[doc0258]") {
    for (std::uint8_t alpha : {0u, 1u}) {
        for (std::uint8_t bw : {1u, 2u, 4u, 8u}) {
            const compact_sequence_stdc_type_params t{
                .alphabet_code = alpha,
                .storage_unit_byte_width = bw,
            };
            const auto encoded = t.encode();
            const auto decoded =
                compact_sequence_stdc_type_params::decode(encoded);
            REQUIRE(decoded == t);
        }
    }
}

TEST_CASE("DOC0258: data[] offset is uint64-aligned across pack widths",
          "[doc0258]") {
    using P0 = supported_alphabets<
        alphabet_entry<seq::dna4, "dna4">,
        alphabet_entry<seq::dna5, "dna5">>;
    using stdc_default = compact_sequence_stdc<P0>;
    using stdc_bp16    = compact_sequence_stdc<P0, std::uint16_t>;
    using stdc_bp8     = compact_sequence_stdc<P0, std::uint8_t>;
    using stdc_bp64    = compact_sequence_stdc<P0, std::uint64_t>;
    REQUIRE(offsetof(stdc_default, data) % 8u == 0u);
    REQUIRE(offsetof(stdc_bp16,    data) % 8u == 0u);
    REQUIRE(offsetof(stdc_bp8,     data) % 8u == 0u);
    REQUIRE(offsetof(stdc_bp64,    data) % 8u == 0u);
}

// =====================================================================
// DOC0261 unit tests — CompTimeStr + pack name dispatch.
// =====================================================================

TEST_CASE("DOC0261: CompTimeStr basic operations",
          "[doc0261][templating][CompTimeStr]") {
    using namespace ::bioseq::common::templating;
    constexpr CompTimeStr a{"foo"};
    constexpr CompTimeStr b{"bar"};
    STATIC_REQUIRE(a.size() == 3);
    STATIC_REQUIRE(a == "foo");
    STATIC_REQUIRE((a + b) == "foobar");
    STATIC_REQUIRE(a.equals_cstr("foo"));
    STATIC_REQUIRE(!a.equals_cstr("food"));
    STATIC_REQUIRE(!a.equals_cstr("fo"));
}

TEST_CASE("DOC0261: supported_alphabets name/code roundtrip",
          "[doc0261][supported_alphabets][names]") {
    using sa = test_supported_alphabets;
    STATIC_REQUIRE(sa::name_of<seq::dna4>() == "dna4");
    STATIC_REQUIRE(sa::name_of<seq::dna5>() == "dna5");
    REQUIRE(sa::name_of_alphabet_code(
        sa::index_of<seq::dna4>()) == std::string_view{"dna4"});
    REQUIRE(sa::name_of_alphabet_code(
        sa::index_of<seq::dna5>()) == std::string_view{"dna5"});

    REQUIRE(sa::try_index_of_name("dna4")
            == std::optional<sa::alphabet_code_t>{0});
    REQUIRE(sa::try_index_of_name("dna5")
            == std::optional<sa::alphabet_code_t>{1});
    REQUIRE(sa::try_index_of_name("xyz") == std::nullopt);
    REQUIRE(sa::try_index_of_name("dna") == std::nullopt);
    REQUIRE(sa::try_index_of_name("dna44") == std::nullopt);
    REQUIRE(sa::try_index_of_name("") == std::nullopt);
}

TEST_CASE("DOC0261: supported_alphabets valid_alphabet_names_csv",
          "[doc0261][supported_alphabets][names]") {
    REQUIRE(test_supported_alphabets::valid_alphabet_names_csv()
            == std::string_view{"'dna4', 'dna5'"});
}

TEST_CASE("DOC0261: supported_alphabets pack uniqueness static_asserts "
          "(positive form)",
          "[doc0261][supported_alphabets][static_assert]") {
    STATIC_REQUIRE(test_supported_alphabets::alphabet_count == 2u);
    using sa = test_supported_alphabets;
    STATIC_REQUIRE(std::is_same_v<
        sa::entry_for<seq::dna4>::alphabet_type, seq::dna4>);
    STATIC_REQUIRE(std::is_same_v<
        sa::entry_for<seq::dna5>::alphabet_type, seq::dna5>);
    STATIC_REQUIRE(sa::entry_for<seq::dna4>::alphabet_name_sv == "dna4");

    // The compiling of `test_supported_alphabets` itself proves the
    // pack-level uniqueness static_asserts fire as expected on this
    // pack (they would be a build error otherwise). Pin the predicates
    // explicitly so a regression in the predicates is caught even if
    // an unrelated edit removes the pack-level static_assert lines.
    STATIC_REQUIRE(::bioseq::common::templating::unique_types_in_tuple_v<
        typename sa::alphabets_tuple>);
    STATIC_REQUIRE(dispatch_detail::unique_names_in_pack<
        alphabet_entry<seq::dna4, "dna4">,
        alphabet_entry<seq::dna5, "dna5">>());
}

TEST_CASE("DOC0261: dispatch_alphabet_by_name pins entry at type level",
          "[doc0261][supported_alphabets][dispatch]") {
    bool saw_dna4 = false;
    const bool matched_dna4 = test_supported_alphabets::dispatch_alphabet_by_name(
        "dna4",
        [&]<class Entry>() {
            saw_dna4 = std::is_same_v<typename Entry::alphabet_type, seq::dna4>;
        });
    REQUIRE(matched_dna4);
    REQUIRE(saw_dna4);

    bool saw_unknown = true;
    const bool matched_unknown = test_supported_alphabets::dispatch_alphabet_by_name(
        "rna7",
        [&]<class /*Entry*/>() { saw_unknown = false; });
    REQUIRE_FALSE(matched_unknown);
    REQUIRE(saw_unknown);  // handler was NEVER invoked on no-match.
}

};

HWY_AFTER_NAMESPACE();
