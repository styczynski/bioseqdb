// DOC0377 §17.3 — exhaustive genericity/parity proof for the one PG-free
// LSB-first packing core and all non-PG adapters.

#include "common_simd.h"
#include <tests/support/common.hpp>
#include <bioseq/pg_build_profile.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

import bioseq.common.sequan_compat;
import bioseq.parallelism.memory_admission;
import bioseq.sequences;
import bioseq.sequences.rank_pack;
import bioseq.sequences.rank_pack_core;
import bioseq.sequences.simd;
import bioseq.storage.lazy;

namespace {

template <class Alphabet>
struct alphabet_stdc_entry;

template <>
struct alphabet_stdc_entry<seq::dna4> {
    using type = alphabet_entry<seq::dna4, "dna4">;
};

template <>
struct alphabet_stdc_entry<seq::dna5> {
    using type = alphabet_entry<seq::dna5, "dna5">;
};

template <>
struct alphabet_stdc_entry<seq::aa20> {
    using type = alphabet_entry<seq::aa20, "aa20">;
};

template <>
struct alphabet_stdc_entry<seq::aa27> {
    using type = alphabet_entry<seq::aa27, "aa27">;
};

template <class Alphabet, class Unit>
using profile_stdc = compact_sequence_stdc<
    supported_alphabets<typename alphabet_stdc_entry<Alphabet>::type>,
    std::uint32_t,
    std::int32_t,
    supported_storage_unit_byte_widths<sizeof(Unit)>>;

template <class Stdc>
struct stdc_test_arena {
    std::vector<std::unique_ptr<std::byte[]>> blocks;

    auto allocator() {
        return [this](std::size_t byte_count) -> Stdc* {
            auto block = std::make_unique<std::byte[]>(byte_count);
            std::memset(block.get(), 0, byte_count);
            auto* result = reinterpret_cast<Stdc*>(block.get());
            blocks.push_back(std::move(block));
            return result;
        };
    }
};

template <class Unit>
void require_unit_bytes_equal(
        std::span<const Unit> actual,
        std::span<const Unit> expected) {
    REQUIRE(actual.size() == expected.size());
    if (!actual.empty()) {
        REQUIRE(std::memcmp(actual.data(), expected.data(),
                            actual.size_bytes()) == 0);
    }
}

template <class Alphabet, class Unit>
[[nodiscard]] std::vector<Unit> manual_pack(
        std::span<const std::uint8_t> ranks) {
    constexpr std::size_t bits =
        seqan3::detail::ceil_log2(seqan3::alphabet_size<Alphabet>);
    constexpr std::size_t letters_per_unit =
        std::numeric_limits<Unit>::digits / bits;
    std::vector<Unit> result(
        (ranks.size() + letters_per_unit - 1u) / letters_per_unit,
        Unit{});
    for (std::size_t index = 0u; index < ranks.size(); ++index) {
        result[index / letters_per_unit] |= static_cast<Unit>(
            static_cast<Unit>(ranks[index])
            << (bits * (index % letters_per_unit)));
    }
    return result;
}

template <class Alphabet>
[[nodiscard]] std::vector<Alphabet> letters_from_ranks(
        std::span<const std::uint8_t> ranks) {
    std::vector<Alphabet> letters(ranks.size());
    for (std::size_t index = 0u; index < ranks.size(); ++index) {
        letters[index].assign_rank(ranks[index]);
    }
    return letters;
}

template <class Alphabet>
[[nodiscard]] std::string text_from_ranks(
        std::span<const std::uint8_t> ranks) {
    const auto letters = letters_from_ranks<Alphabet>(ranks);
    std::string text;
    text.reserve(letters.size());
    for (const auto letter : letters) {
        text.push_back(letter.to_char());
    }
    return text;
}

template <class Alphabet, class Unit>
void require_stdc_io_oracle(
        std::span<const std::uint8_t> ranks,
        std::span<const Unit> expected,
        std::uint16_t padding,
        std::string_view expected_text) {
    using stdc_type = profile_stdc<Alphabet, Unit>;
    struct cursor {
        std::span<const Unit> units;
        std::size_t index{};
    } source{expected};
    auto read_unit = []<class, class Out>(cursor& input,
                                          std::uint8_t byte_width) -> Out {
        REQUIRE(byte_width == sizeof(Unit));
        REQUIRE(input.index < input.units.size());
        return static_cast<Out>(input.units[input.index++]);
    };

    stdc_test_arena<stdc_type> arena;
    auto* value = stdc_type::template convert_from<Alphabet>(
        sizeof(Unit),
        padding,
        static_cast<typename stdc_type::bp_size_t>(ranks.size()),
        static_cast<std::uint32_t>(expected.size()),
        source,
        read_unit,
        arena.allocator());
    REQUIRE(value != nullptr);
    REQUIRE(source.index == expected.size());
    REQUIRE(value->unit_count() == expected.size());
    REQUIRE(value->padding_letters == padding);
    REQUIRE(value->storage_unit_byte_width == sizeof(Unit));
    if (!expected.empty()) {
        REQUIRE(std::memcmp(value->data, expected.data(),
                            expected.size_bytes()) == 0);
    }

    std::vector<std::uint64_t> sent;
    value->send_to(
        sent,
        []<class In>(std::vector<std::uint64_t>& output,
                     std::uint64_t unit,
                     std::uint8_t byte_width) {
            static_assert(std::is_same_v<In, std::uint64_t>);
            REQUIRE(byte_width == sizeof(Unit));
            output.push_back(unit);
        });
    REQUIRE(sent.size() == expected.size());
    for (std::size_t index = 0u; index < sent.size(); ++index) {
        REQUIRE(sent[index] == static_cast<std::uint64_t>(expected[index]));
    }

    std::string converted_text;
    const std::size_t rank_count = ranks.size();
    value->convert_to(
        [&converted_text, rank_count, padding]<class Instance>(
                const compact_sequence_range<Instance>& range) {
            converted_text = range.to_string();
            REQUIRE(range.bp_size() == rank_count);
            REQUIRE(range.padding() == padding);
        });
    REQUIRE(converted_text == expected_text);
}

template <class Alphabet, class Unit>
void require_admitted_oracle(
        std::span<const std::uint8_t> ranks,
        std::span<const Unit> expected,
        std::uint16_t padding) {
    using instance = sequence_custom_alphabet<
        Alphabet,
        Unit,
        seqan3::detail::ceil_log2(seqan3::alphabet_size<Alphabet>)>;

    std::vector<std::uint8_t> guarded_ranks;
    guarded_ranks.reserve(ranks.size() + 2u);
    guarded_ranks.push_back(static_cast<std::uint8_t>(
        seqan3::alphabet_size<Alphabet> - 1u));
    guarded_ranks.insert(guarded_ranks.end(), ranks.begin(), ranks.end());
    guarded_ranks.push_back(0u);
    auto guarded_letters = letters_from_ranks<Alphabet>(guarded_ranks);
    compact_sequence_range<instance> guarded_range{guarded_letters};
    const auto source = bioseq::storage::as_lazy_sequence_view(guarded_range);

    const auto admission_bytes =
        bioseq::storage::packed_region_admission_bytes<Alphabet>(
            ranks.size(), sizeof(Unit));
    auto& admission =
        bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t baseline = admission.reserved_bytes();
    auto permit = admission.try_reserve(admission_bytes);
    REQUIRE(permit.has_value());
    {
        auto owner = bioseq::storage::materialize_packed_region(
            source,
            1u,
            1u + ranks.size(),
            sizeof(Unit),
            std::move(*permit));
        const auto& descriptor = owner.descriptor();
        REQUIRE(admission.reserved_bytes() == baseline + admission_bytes);
        REQUIRE(descriptor.bp_length == ranks.size());
        REQUIRE(descriptor.unit_count == expected.size());
        REQUIRE(descriptor.storage_unit_byte_width == sizeof(Unit));
        REQUIRE(descriptor.bits_per_symbol
                == seqan3::detail::ceil_log2(
                    seqan3::alphabet_size<Alphabet>));
        REQUIRE(descriptor.leading_symbol_offset == 0u);
        REQUIRE(descriptor.trailing_padding_letters == padding);
        REQUIRE(descriptor.bit_order
                == bioseq::storage::basic_lazy_sequence_view<Alphabet>::
                    packed_bit_order::least_significant_symbol_first);
        if (!expected.empty()) {
            REQUIRE(descriptor.units != nullptr);
            REQUIRE(std::memcmp(descriptor.units, expected.data(),
                                expected.size_bytes()) == 0);
        }

        std::vector<std::uint8_t> observed(ranks.size());
        owner.view().template to_encoded_into<false, false>(
            observed, 0u, observed.size());
        REQUIRE(observed
                == std::vector<std::uint8_t>(ranks.begin(), ranks.end()));
    }
    REQUIRE(admission.reserved_bytes() == baseline);
}

template <class Alphabet, class Unit>
void require_invalid_and_capacity_matrix(std::string_view profile) {
    constexpr std::size_t bits =
        seqan3::detail::ceil_log2(seqan3::alphabet_size<Alphabet>);
    constexpr std::size_t alphabet_size = seqan3::alphabet_size<Alphabet>;
    constexpr std::size_t letters_per_unit =
        std::numeric_limits<Unit>::digits / bits;
    using instance = sequence_custom_alphabet<Alphabet, Unit, bits>;
    using grow_sink = bioseq::sequences::growing_rank_pack_unit_sink<Unit>;
    using fixed_sink = bioseq::sequences::fixed_rank_pack_unit_sink<Unit>;
    using reject_grow = bioseq::sequences::checked_rank_pack_sink<
        instance,
        bioseq::sequences::invalid_rank_policy::reject,
        bioseq::sequences::pack_capacity_policy::grow_vector,
        grow_sink>;
    using drop_grow = bioseq::sequences::checked_rank_pack_sink<
        instance,
        bioseq::sequences::invalid_rank_policy::drop,
        bioseq::sequences::pack_capacity_policy::grow_vector,
        grow_sink>;
    using drop_fixed = bioseq::sequences::checked_rank_pack_sink<
        instance,
        bioseq::sequences::invalid_rank_policy::drop,
        bioseq::sequences::pack_capacity_policy::fixed_span,
        fixed_sink>;
    using reject_fixed = bioseq::sequences::checked_rank_pack_sink<
        instance,
        bioseq::sequences::invalid_rank_policy::reject,
        bioseq::sequences::pack_capacity_policy::fixed_span,
        fixed_sink>;

    const std::array<std::uint8_t, 2u> valid{
        0u, static_cast<std::uint8_t>(alphabet_size - 1u)};
    const auto expected_drop = manual_pack<Alphabet, Unit>(valid);
    const std::uint16_t expected_drop_padding =
        valid.size() % letters_per_unit == 0u
        ? 0u
        : static_cast<std::uint16_t>(
            letters_per_unit - valid.size() % letters_per_unit);

    for (const std::uint8_t invalid_rank : {
            static_cast<std::uint8_t>(alphabet_size),
            std::uint8_t{0xffu}}) {
        const std::array<std::uint8_t, 3u> invalid{
            valid.front(), invalid_rank, valid.back()};
        for (std::size_t split = 0u; split <= invalid.size(); ++split) {
            CAPTURE(profile, invalid_rank, split);
            const auto input = std::span<const std::uint8_t>{invalid};

            std::vector<Unit> rejected_units;
            reject_grow rejected{grow_sink{rejected_units}};
            static_cast<void>(rejected.append_ranks(input.first(split)));
            static_cast<void>(rejected.append_ranks(input.subspan(split)));
            const auto rejected_result = rejected.finish();
            REQUIRE(rejected_result.status
                    == bioseq::sequences::rank_pack_status::invalid_rank);
            REQUIRE(rejected_result.emitted_symbols == 1u);

            std::vector<Unit> dropped_units;
            drop_grow dropped{grow_sink{dropped_units}};
            REQUIRE(dropped.append_ranks(input.first(split))
                    == bioseq::sequences::rank_pack_status::ok);
            REQUIRE(dropped.append_ranks(input.subspan(split))
                    == bioseq::sequences::rank_pack_status::ok);
            const auto dropped_result = dropped.finish(
                valid.size(), expected_drop.size());
            REQUIRE(dropped_result.status
                    == bioseq::sequences::rank_pack_status::ok);
            REQUIRE(dropped_result.padding_letters == expected_drop_padding);
            require_unit_bytes_equal<Unit>(dropped_units, expected_drop);

            std::vector<Unit> fixed_drop_units(expected_drop.size());
            drop_fixed fixed_drop{fixed_sink{fixed_drop_units}};
            REQUIRE(fixed_drop.append_ranks(input.first(split))
                    == bioseq::sequences::rank_pack_status::ok);
            REQUIRE(fixed_drop.append_ranks(input.subspan(split))
                    == bioseq::sequences::rank_pack_status::ok);
            const auto fixed_drop_result = fixed_drop.finish(
                valid.size(), expected_drop.size());
            REQUIRE(fixed_drop_result.status
                    == bioseq::sequences::rank_pack_status::ok);
            REQUIRE(fixed_drop_result.padding_letters
                    == expected_drop_padding);
            require_unit_bytes_equal<Unit>(fixed_drop_units, expected_drop);
        }
    }

    std::vector<std::uint8_t> capacity_ranks(letters_per_unit + 1u);
    for (std::size_t index = 0u; index < capacity_ranks.size(); ++index) {
        capacity_ranks[index] = static_cast<std::uint8_t>(index % alphabet_size);
    }
    const auto capacity_expected =
        manual_pack<Alphabet, Unit>(capacity_ranks);
    REQUIRE(capacity_expected.size() == 2u);
    for (std::size_t split = 0u; split <= capacity_ranks.size(); ++split) {
        CAPTURE(profile, split);
        const auto input = std::span<const std::uint8_t>{capacity_ranks};

        std::vector<Unit> exact_units(capacity_expected.size());
        reject_fixed exact{fixed_sink{exact_units}};
        REQUIRE(exact.append_ranks(input.first(split))
                == bioseq::sequences::rank_pack_status::ok);
        REQUIRE(exact.append_ranks(input.subspan(split))
                == bioseq::sequences::rank_pack_status::ok);
        const auto exact_result = exact.finish(
            capacity_ranks.size(), capacity_expected.size());
        REQUIRE(exact_result.status
                == bioseq::sequences::rank_pack_status::ok);
        require_unit_bytes_equal<Unit>(exact_units, capacity_expected);

        std::vector<Unit> short_units(capacity_expected.size() - 1u);
        reject_fixed short_writer{fixed_sink{short_units}};
        static_cast<void>(short_writer.append_ranks(input.first(split)));
        static_cast<void>(short_writer.append_ranks(input.subspan(split)));
        const auto short_result = short_writer.finish();
        REQUIRE(short_result.status
                == bioseq::sequences::rank_pack_status::capacity_overflow);
        REQUIRE(short_result.written_units == short_units.size());
    }
}

template <class Alphabet, class Unit>
void require_exhaustive_profile(std::string_view profile) {
    constexpr std::size_t bits =
        seqan3::detail::ceil_log2(seqan3::alphabet_size<Alphabet>);
    constexpr std::size_t alphabet_size = seqan3::alphabet_size<Alphabet>;
    constexpr std::size_t letters_per_unit =
        std::numeric_limits<Unit>::digits / bits;
    using instance = sequence_custom_alphabet<Alphabet, Unit, bits>;
    using grow_sink = bioseq::sequences::growing_rank_pack_unit_sink<Unit>;
    using fixed_sink = bioseq::sequences::fixed_rank_pack_unit_sink<Unit>;
    using grow_writer = bioseq::sequences::checked_rank_pack_sink<
        instance,
        bioseq::sequences::invalid_rank_policy::reject,
        bioseq::sequences::pack_capacity_policy::grow_vector,
        grow_sink>;
    using fixed_writer = bioseq::sequences::checked_rank_pack_sink<
        instance,
        bioseq::sequences::invalid_rank_policy::reject,
        bioseq::sequences::pack_capacity_policy::fixed_span,
        fixed_sink>;

    for (std::size_t length = 0u; length <= letters_per_unit + 1u; ++length) {
        std::vector<std::uint8_t> ranks(length);
        for (std::size_t index = 0u; index < length; ++index) {
            ranks[index] = static_cast<std::uint8_t>(
                (index * 7u + index / 3u) % alphabet_size);
        }
        const auto expected = manual_pack<Alphabet, Unit>(ranks);
        auto letters = letters_from_ranks<Alphabet>(ranks);
        const auto expected_text = text_from_ranks<Alphabet>(ranks);
        const std::uint16_t expected_padding =
            length == 0u || length % letters_per_unit == 0u
            ? 0u
            : static_cast<std::uint16_t>(
                letters_per_unit - length % letters_per_unit);

        compact_sequence_range<instance> legacy{letters};
        CAPTURE(profile, length);
        require_unit_bytes_equal<Unit>(legacy._data, expected);
        REQUIRE(legacy.padding() == expected_padding);
        REQUIRE(legacy.bp_size() == length);
        REQUIRE(legacy.to_string() == expected_text);

        const auto scalar =
            bioseq::simd::active_target::encode_sequence_scalar<instance>(
                expected_text.data(), expected_text.size());
        const auto simd =
            bioseq::simd::active_target::encode_sequence_simd<instance>(
                expected_text.data(), expected_text.size());
        require_unit_bytes_equal<Unit>(scalar._data, expected);
        require_unit_bytes_equal<Unit>(simd._data, expected);
        REQUIRE(scalar.padding() == expected_padding);
        REQUIRE(simd.padding() == expected_padding);

        require_stdc_io_oracle<Alphabet, Unit>(
            ranks, expected, expected_padding, expected_text);
        require_admitted_oracle<Alphabet, Unit>(
            ranks, expected, expected_padding);

        for (std::size_t split = 0u; split <= length; ++split) {
            CAPTURE(profile, length, split);
            const auto input = std::span<const std::uint8_t>{ranks};

            std::vector<Unit> grown;
            grow_writer growing{grow_sink{grown}};
            REQUIRE(growing.append_ranks(input.first(split))
                    == bioseq::sequences::rank_pack_status::ok);
            REQUIRE(growing.append_ranks(input.subspan(split))
                    == bioseq::sequences::rank_pack_status::ok);
            const auto grow_result = growing.finish(length, expected.size());
            REQUIRE(grow_result.status
                    == bioseq::sequences::rank_pack_status::ok);
            REQUIRE(grow_result.written_units == expected.size());
            REQUIRE(grow_result.padding_letters == expected_padding);
            require_unit_bytes_equal<Unit>(grown, expected);

            std::vector<Unit> fixed(expected.size());
            fixed_writer fixed_output{fixed_sink{fixed}};
            REQUIRE(fixed_output.append_ranks(input.first(split))
                    == bioseq::sequences::rank_pack_status::ok);
            REQUIRE(fixed_output.append_ranks(input.subspan(split))
                    == bioseq::sequences::rank_pack_status::ok);
            const auto fixed_result = fixed_output.finish(
                length, expected.size());
            REQUIRE(fixed_result.status
                    == bioseq::sequences::rank_pack_status::ok);
            REQUIRE(fixed_result.written_units == expected.size());
            REQUIRE(fixed_result.padding_letters == expected_padding);
            require_unit_bytes_equal<Unit>(fixed, expected);

            std::vector<Unit> streamed;
            bioseq::simd::active_target::encode_state<
                Unit, bits, letters_per_unit> stream_state{};
            stream_state.output_storage = &streamed;
            stream_state.reset();
            bioseq::simd::active_target::encode_body_chunk<
                Alphabet,
                bioseq::simd::active_target::unknown_byte_policy::
                    MAP_TO_DEFAULT,
                Unit,
                bits,
                letters_per_unit>(
                    stream_state,
                    reinterpret_cast<const std::uint8_t*>(
                        expected_text.data()),
                    split);
            bioseq::simd::active_target::encode_body_chunk<
                Alphabet,
                bioseq::simd::active_target::unknown_byte_policy::
                    MAP_TO_DEFAULT,
                Unit,
                bits,
                letters_per_unit>(
                    stream_state,
                    reinterpret_cast<const std::uint8_t*>(
                        expected_text.data() + split),
                    expected_text.size() - split);
            const auto [stream_units, stream_padding] =
                bioseq::simd::active_target::finalize_encode_state<
                    Unit, bits, letters_per_unit>(stream_state);
            REQUIRE(stream_units == expected.size());
            REQUIRE(stream_padding == expected_padding);
            require_unit_bytes_equal<Unit>(streamed, expected);
        }
    }

    require_invalid_and_capacity_matrix<Alphabet, Unit>(profile);
}

}  // namespace

TEST_CASE("DOC0377 §17.3 — PG-free rank packing is exhaustive across profiles",
          "[doc0377][rank-pack][genericity][unit]") {
    // Level 0 remains the fast compiler/iteration profile while still covering
    // the canonical SIMD and PG width. Any final profile instantiates all
    // supported no-PG alphabets and widths.
    require_exhaustive_profile<seq::dna4, std::uint64_t>("dna4/u64");

#if BIOSEQ_OPTTIMIZATION_LEVEL > 0
    require_exhaustive_profile<seq::dna4, std::uint8_t>("dna4/u8");
    require_exhaustive_profile<seq::dna4, std::uint16_t>("dna4/u16");
    require_exhaustive_profile<seq::dna4, std::uint32_t>("dna4/u32");

    require_exhaustive_profile<seq::dna5, std::uint8_t>("dna5/u8");
    require_exhaustive_profile<seq::dna5, std::uint16_t>("dna5/u16");
    require_exhaustive_profile<seq::dna5, std::uint32_t>("dna5/u32");
    require_exhaustive_profile<seq::dna5, std::uint64_t>("dna5/u64");

    require_exhaustive_profile<seq::aa20, std::uint8_t>("aa20/u8");
    require_exhaustive_profile<seq::aa20, std::uint16_t>("aa20/u16");
    require_exhaustive_profile<seq::aa20, std::uint32_t>("aa20/u32");
    require_exhaustive_profile<seq::aa20, std::uint64_t>("aa20/u64");

    require_exhaustive_profile<seq::aa27, std::uint8_t>("aa27/u8");
    require_exhaustive_profile<seq::aa27, std::uint16_t>("aa27/u16");
    require_exhaustive_profile<seq::aa27, std::uint32_t>("aa27/u32");
    require_exhaustive_profile<seq::aa27, std::uint64_t>("aa27/u64");
#endif
}
