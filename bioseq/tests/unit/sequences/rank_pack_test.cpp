/**
 * @file
 * @brief Compare rank packing with independently packed bytes across alphabets and storage widths.
 */

#include <cstring>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.common.seqan_adapter;
import bioseq.parallelism.memory_admission;
import bioseq.sequences;
import bioseq.sequences.rank_pack;
import bioseq.sequences.rank_pack_core;
import bioseq.sequences.simd;
import bioseq.storage.lazy_sequence;

namespace {

/** Select the packed-sequence alphabet entry used by a test. */
template <class p_alphabet> struct alphabet_stdc_entry;

/** DNA4 entry for the test sequence type. */
template <> struct alphabet_stdc_entry<seq::dna4> {
    using type = alphabet_entry<seq::dna4, "dna4">;
};

/** DNA5 entry for the test sequence type. */
template <> struct alphabet_stdc_entry<seq::dna5> {
    using type = alphabet_entry<seq::dna5, "dna5">;
};

/** Entry for the 20-letter amino-acid alphabet. */
template <> struct alphabet_stdc_entry<seq::aa20> {
    using type = alphabet_entry<seq::aa20, "aa20">;
};

/** Entry for the 27-letter amino-acid alphabet. */
template <> struct alphabet_stdc_entry<seq::aa27> {
    using type = alphabet_entry<seq::aa27, "aa27">;
};

/**
 * @brief Restrict a test sequence type to one alphabet and packed-word width.
 * @tparam p_alphabet Sequence  alphabt.
 * @tparam p_unit Packed word type.
 */
template <class p_alphabet, class p_unit>
using profile_stdc =
    compact_sequence_stdc<supported_alphabets<typename alphabet_stdc_entry<p_alphabet>::type>,
                          std::uint32_t,
                          std::int32_t,
                          supported_storage_unit_byte_widths<sizeof(p_unit)>>;

/**
 * @brief Keep allocated packed-sequence buffers alive for a test.
 * @tparam p_stdc Header type returned by the allocator.
 */
template <class p_stdc> struct stdc_test_arena {
    std::vector<std::unique_ptr<std::byte[]>> blocks;

    /** Return an allocator that stores each zeroed buffer in this arena. */
    [[nodiscard]] auto allocator() {
        return [this](std::size_t byte_count) -> p_stdc* {
            auto block = std::make_unique<std::byte[]>(byte_count);
            std::memset(block.get(), 0, byte_count);
            auto* result = reinterpret_cast<p_stdc*>(block.get());
            blocks.push_back(std::move(block));
            return result;
        };
    }
};

/**
 * @brief Require equal lengths and bytes in two packed-unit spans.
 * @param[in] actual Packed units produced by the code under test.
 * @param[in] expected Independently packed reference units.
 */
template <class p_unit>
void require_unit_bytes_equal(std::span<const p_unit> actual, std::span<const p_unit> expected) {
    REQUIRE(actual.size() == expected.size());
    if (!actual.empty()) {
        REQUIRE(std::memcmp(actual.data(), expected.data(), actual.size_bytes()) == 0);
    }
}

/**
 * @brief Pack ranks independently, starting at the low bits of each word.
 * @tparam p_alphabet Alphabet defining bits per symbol.
 * @tparam p_unit Packed word type
 * @param[in] ranks Valid alphabet ranks.
 * @return Packed words with unused bits set to  zero.
 */
template <class p_alphabet, class p_unit>
[[nodiscard]] std::vector<p_unit> manual_pack(std::span<const std::uint8_t> ranks) {
    constexpr std::size_t bits = seqan3::detail::ceil_log2(seqan3::alphabet_size<p_alphabet>);
    constexpr std::size_t letters_per_unit = std::numeric_limits<p_unit>::digits / bits;
    std::vector<p_unit> result((ranks.size() + letters_per_unit - 1u) / letters_per_unit, p_unit{});
    for (std::size_t index = 0u; index < ranks.size(); ++index) {
        result[index / letters_per_unit] |= static_cast<p_unit>(
            static_cast<p_unit>(ranks[index]) << (bits * (index % letters_per_unit)));
    }
    return result;
}

/**
 * @brief Convert valid ranks to alphabet symbols in the same order.
 * @tparam p_alphabet Alphabet containing every input rank.
 */
template <class p_alphabet>
[[nodiscard]] std::vector<p_alphabet> letters_from_ranks(std::span<const std::uint8_t> ranks) {
    std::vector<p_alphabet> letters(ranks.size());
    for (std::size_t index = 0u; index < ranks.size(); ++index) {
        letters[index].assign_rank(ranks[index]);
    }
    return letters;
}

/**
 * @brief Decode valid ranks to text in the same order.
 * @tparam p_alphabet Alphabet containing every input rank.
 */
template <class p_alphabet>
[[nodiscard]] std::string text_from_ranks(std::span<const std::uint8_t> ranks) {
    const auto letters = letters_from_ranks<p_alphabet>(ranks);
    std::string text;
    text.reserve(letters.size());
    for (const auto letter : letters) {
        text.push_back(letter.to_char());
    }
    return text;
}

/**
 * @brief Check receive, send, and typed views against independent packed data.
 * @tparam p_alphabet Sequence alphabet.
 * @tparam p_unit Packed word type.
 * @param[in] ranks Source ranks.
 * @param[in] expected Reference packed words.
 * @param[in] padding Unused symbol positions in the last word.
 * @param[in] expected_text Reference text after decoding.
 */
template <class p_alphabet, class p_unit>
void require_stdc_io_oracle(std::span<const std::uint8_t> ranks,
                            std::span<const p_unit> expected,
                            std::uint16_t padding,
                            std::string_view expected_text) {
    using stdc_type = profile_stdc<p_alphabet, p_unit>;
    /// Track the next reference word supplied to the receiver.
    struct cursor {
        std::span<const p_unit> units;
        std::size_t index{};
    } source{expected};
    auto read_unit = []<class, class p_output>(cursor& input, std::uint8_t byte_width) -> p_output {
        REQUIRE(byte_width == sizeof(p_unit));
        REQUIRE(input.index < input.units.size());
        return static_cast<p_output>(input.units[input.index++]);
    };

    stdc_test_arena<stdc_type> arena;
    auto* value = stdc_type::template convert_from<p_alphabet>(
        sizeof(p_unit),
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
    REQUIRE(value->storage_unit_byte_width == sizeof(p_unit));
    if (!expected.empty()) {
        REQUIRE(std::memcmp(value->data, expected.data(), expected.size_bytes()) == 0);
    }

    std::vector<std::uint64_t> sent;
    value->send_to(sent,
                   []<class p_input>(std::vector<std::uint64_t>& output,
                                     std::uint64_t unit,
                                     std::uint8_t byte_width) {
                       static_assert(std::is_same_v<p_input, std::uint64_t>);
                       REQUIRE(byte_width == sizeof(p_unit));
                       output.push_back(unit);
                   });
    REQUIRE(sent.size() == expected.size());
    for (std::size_t index = 0u; index < sent.size(); ++index) {
        REQUIRE(sent[index] == static_cast<std::uint64_t>(expected[index]));
    }

    std::string converted_text;
    const std::size_t rank_count = ranks.size();
    value->convert_to([&converted_text, rank_count, padding]<class p_instance>(
                          const compact_sequence_range<p_instance>& range) {
        converted_text = range.to_string();
        REQUIRE(range.bp_size() == rank_count);
        REQUIRE(range.padding() == padding);
    });
    REQUIRE(converted_text == expected_text);
}

/**
 * @brief Check repacked bytes and the memory budget,  reserved for them.
 *
 * Copy a slice into the requested word width, compare its bytes and padding,
 * and check tlat destroying the owner returns its reservation.
 * @tparam p_alphabet Sequence alphabet.
 * @tparam p_unit Packed word type.
 * @param[in] ranks Source ranks.
 * @param[in] expected Reference packed words.
 * @param[in] padding Unused symbol positions in the last word.
 */
template <class p_alphabet, class p_unit>
void require_admitted_oracle(std::span<const std::uint8_t> ranks,
                             std::span<const p_unit> expected,
                             std::uint16_t padding) {
    using instance =
        sequence_custom_alphabet<p_alphabet,
                                 p_unit,
                                 seqan3::detail::ceil_log2(seqan3::alphabet_size<p_alphabet>)>;

    std::vector<std::uint8_t> guarded_ranks;
    guarded_ranks.reserve(ranks.size() + 2u);
    guarded_ranks.push_back(static_cast<std::uint8_t>(seqan3::alphabet_size<p_alphabet> - 1u));
    guarded_ranks.insert(guarded_ranks.end(), ranks.begin(), ranks.end());
    guarded_ranks.push_back(0u);
    auto guarded_letters = letters_from_ranks<p_alphabet>(guarded_ranks);
    compact_sequence_range<instance> guarded_range{guarded_letters};
    const auto source = bioseq::storage::make_lazy_sequence_view(guarded_range);

    const auto physical_plan =
        bioseq::storage::plan_packed_sequence_allocation<p_alphabet>(
            ranks.size(), sizeof(p_unit));
    const auto admission_bytes = physical_plan.total_allocation_requirement_bytes;
    REQUIRE(admission_bytes ==
            bioseq::storage::packed_sequence_admission_bytes<p_alphabet>(
                ranks.size(), sizeof(p_unit)));
    REQUIRE(physical_plan.logical_payload_bytes == expected.size_bytes());
    REQUIRE(physical_plan.payload_highway_request_bound_bytes >=
            physical_plan.logical_payload_bytes);
    REQUIRE(physical_plan.payload_allocator_request_bytes >=
            physical_plan.payload_highway_request_bound_bytes);
    REQUIRE(physical_plan.payload_allocation_requirement_bytes >=
            physical_plan.payload_allocator_request_bytes);
    REQUIRE(physical_plan.control_object_bytes > 0u);
    REQUIRE(physical_plan.control_allocator_request_bytes >=
            physical_plan.control_object_bytes);
    REQUIRE(physical_plan.control_allocation_requirement_bytes >=
            physical_plan.control_allocator_request_bytes);
    REQUIRE(admission_bytes == physical_plan.payload_allocation_requirement_bytes +
                                   physical_plan.control_allocation_requirement_bytes);
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t baseline = admission.reserved_bytes();
    auto permit = admission.try_reserve(admission_bytes);
    REQUIRE(permit.has_value());
    {
        auto owner = bioseq::storage::materialize_packed_sequence(
            source,
            1u,
            1u + ranks.size(),
            sizeof(p_unit),
            std::move(*permit),
            bioseq::storage::packed_materialization_policy::allow_rank_repack,
            /*no footprint reflection=*/nullptr);
        const auto& descriptor = owner.descriptor();
        const auto payload_allocation = owner.test_payload_allocation_bytes();
        const auto control_allocation = owner.test_control_allocation_bytes();
        REQUIRE(payload_allocation >= physical_plan.payload_allocator_request_bytes);
        REQUIRE(payload_allocation <= physical_plan.payload_allocation_requirement_bytes);
        REQUIRE(control_allocation >= physical_plan.control_allocator_request_bytes);
        REQUIRE(control_allocation <= physical_plan.control_allocation_requirement_bytes);
        REQUIRE(owner.test_admission_bytes() == payload_allocation + control_allocation);
        REQUIRE(owner.test_admission_bytes() <= admission_bytes);
#if defined(__APPLE__)
        REQUIRE(physical_plan.payload_allocation_requirement_bytes ==
                (physical_plan.payload_allocator_request_bytes == 0u
                     ? 0u
                     : 2u * physical_plan.payload_allocator_request_bytes - 1u));
        REQUIRE(physical_plan.control_allocation_requirement_bytes ==
                2u * physical_plan.control_allocator_request_bytes - 1u);
#endif
        REQUIRE(admission.reserved_bytes() == baseline + owner.test_admission_bytes());
        REQUIRE(descriptor.logical_length == ranks.size());
        REQUIRE(descriptor.unit_count == expected.size());
        REQUIRE(descriptor.storage_unit_width == sizeof(p_unit));
        REQUIRE(descriptor.bits_per_symbol ==
                seqan3::detail::ceil_log2(seqan3::alphabet_size<p_alphabet>));
        REQUIRE(descriptor.leading_symbol_offset == 0u);
        REQUIRE(descriptor.trailing_padding == padding);
        REQUIRE(descriptor.bit_order ==
                bioseq::storage::lazy_sequence_view<
                    p_alphabet>::packed_symbol_order::least_significant_symbol_first);
        if (!expected.empty()) {
            REQUIRE(descriptor.units != nullptr);
            REQUIRE(std::memcmp(descriptor.units, expected.data(), expected.size_bytes()) == 0);
        }

        std::vector<std::uint8_t> observed(ranks.size());
        owner.view().template copy_ranks<false, false>(observed, 0u, observed.size());
        REQUIRE(observed == std::vector<std::uint8_t>(ranks.begin(), ranks.end()));
    }
    REQUIRE(admission.reserved_bytes() == baseline);
}

/**
 * @brief Check rejected and skipped invalid ranks, plus fixed-buffer overflow.
 *
 * Try every split point so these results also hold when input arrives in
 * two pieces.
 * @tparam p_alphabet Sequence alphabet.
 * @tparam p_unit Packed word type.
 * @param[in] profile Name included in failure diagnostics.
 */
template <class p_alphabet, class p_unit>
void require_invalid_and_capacity_matrix(std::string_view profile) {
    constexpr std::size_t bits = seqan3::detail::ceil_log2(seqan3::alphabet_size<p_alphabet>);
    constexpr std::size_t alphabet_size = seqan3::alphabet_size<p_alphabet>;
    constexpr std::size_t letters_per_unit = std::numeric_limits<p_unit>::digits / bits;
    using instance = sequence_custom_alphabet<p_alphabet, p_unit, bits>;
    using grow_sink = bioseq::sequences::growing_rank_pack_unit_sink<p_unit>;
    using fixed_sink = bioseq::sequences::fixed_rank_pack_unit_sink<p_unit>;
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

    const std::array<std::uint8_t, 2u> valid{0u, static_cast<std::uint8_t>(alphabet_size - 1u)};
    const auto expected_drop = manual_pack<p_alphabet, p_unit>(valid);
    const std::uint16_t expected_drop_padding =
        valid.size() % letters_per_unit == 0u
            ? 0u
            : static_cast<std::uint16_t>(letters_per_unit - valid.size() % letters_per_unit);

    for (const std::uint8_t invalid_rank :
         {static_cast<std::uint8_t>(alphabet_size), std::uint8_t{0xffu}}) {
        const std::array<std::uint8_t, 3u> invalid{valid.front(), invalid_rank, valid.back()};
        for (std::size_t split = 0u; split <= invalid.size(); ++split) {
            CAPTURE(profile, invalid_rank, split);
            const auto input = std::span<const std::uint8_t>{invalid};

            std::vector<p_unit> rejected_units;
            reject_grow rejected{grow_sink{rejected_units}};
            static_cast<void>(rejected.append_ranks(input.first(split)));
            static_cast<void>(rejected.append_ranks(input.subspan(split)));
            const auto rejected_result = rejected.finish();
            REQUIRE(rejected_result.status == bioseq::sequences::rank_pack_status::invalid_rank);
            REQUIRE(rejected_result.emitted_symbols == 1u);

            std::vector<p_unit> dropped_units;
            drop_grow dropped{grow_sink{dropped_units}};
            REQUIRE(dropped.append_ranks(input.first(split)) ==
                    bioseq::sequences::rank_pack_status::ok);
            REQUIRE(dropped.append_ranks(input.subspan(split)) ==
                    bioseq::sequences::rank_pack_status::ok);
            const auto dropped_result = dropped.finish(valid.size(), expected_drop.size());
            REQUIRE(dropped_result.status == bioseq::sequences::rank_pack_status::ok);
            REQUIRE(dropped_result.padding_letters == expected_drop_padding);
            require_unit_bytes_equal<p_unit>(dropped_units, expected_drop);

            std::vector<p_unit> fixed_drop_units(expected_drop.size());
            drop_fixed fixed_drop{fixed_sink{fixed_drop_units}};
            REQUIRE(fixed_drop.append_ranks(input.first(split)) ==
                    bioseq::sequences::rank_pack_status::ok);
            REQUIRE(fixed_drop.append_ranks(input.subspan(split)) ==
                    bioseq::sequences::rank_pack_status::ok);
            const auto fixed_drop_result = fixed_drop.finish(valid.size(), expected_drop.size());
            REQUIRE(fixed_drop_result.status == bioseq::sequences::rank_pack_status::ok);
            REQUIRE(fixed_drop_result.padding_letters == expected_drop_padding);
            require_unit_bytes_equal<p_unit>(fixed_drop_units, expected_drop);
        }
    }

    std::vector<std::uint8_t> capacity_ranks(letters_per_unit + 1u);
    for (std::size_t index = 0u; index < capacity_ranks.size(); ++index) {
        capacity_ranks[index] = static_cast<std::uint8_t>(index % alphabet_size);
    }
    const auto capacity_expected = manual_pack<p_alphabet, p_unit>(capacity_ranks);
    REQUIRE(capacity_expected.size() == 2u);
    for (std::size_t split = 0u; split <= capacity_ranks.size(); ++split) {
        CAPTURE(profile, split);
        const auto input = std::span<const std::uint8_t>{capacity_ranks};

        std::vector<p_unit> exact_units(capacity_expected.size());
        reject_fixed exact{fixed_sink{exact_units}};
        REQUIRE(exact.append_ranks(input.first(split)) == bioseq::sequences::rank_pack_status::ok);
        REQUIRE(exact.append_ranks(input.subspan(split)) ==
                bioseq::sequences::rank_pack_status::ok);
        const auto exact_result = exact.finish(capacity_ranks.size(), capacity_expected.size());
        REQUIRE(exact_result.status == bioseq::sequences::rank_pack_status::ok);
        require_unit_bytes_equal<p_unit>(exact_units, capacity_expected);

        std::vector<p_unit> short_units(capacity_expected.size() - 1u);
        reject_fixed short_writer{fixed_sink{short_units}};
        static_cast<void>(short_writer.append_ranks(input.first(split)));
        static_cast<void>(short_writer.append_ranks(input.subspan(split)));
        const auto short_result = short_writer.finish();
        REQUIRE(short_result.status == bioseq::sequences::rank_pack_status::capacity_overflow);
        REQUIRE(short_result.written_units == short_units.size());
    }
}

/**
 * @brief Compare packing methods at every length through one word plus one symbol.
 *
 * Test all  two-piece splits at each length, then check invalid ranks and
 * fixed-buffer overflow
 * @tparam p_alphabet Sequence alphabet.
 * @tparam p_unit Packed word type.
 * @param[in] profile Name included in failure diagnostics.
 */
template <class p_alphabet, class p_unit>
void require_exhaustive_profile(std::string_view profile) {
    constexpr std::size_t bits = seqan3::detail::ceil_log2(seqan3::alphabet_size<p_alphabet>);
    constexpr std::size_t alphabet_size = seqan3::alphabet_size<p_alphabet>;
    constexpr std::size_t letters_per_unit = std::numeric_limits<p_unit>::digits / bits;
    using instance = sequence_custom_alphabet<p_alphabet, p_unit, bits>;
    using grow_sink = bioseq::sequences::growing_rank_pack_unit_sink<p_unit>;
    using fixed_sink = bioseq::sequences::fixed_rank_pack_unit_sink<p_unit>;
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
            ranks[index] = static_cast<std::uint8_t>((index * 7u + index / 3u) % alphabet_size);
        }
        const auto expected = manual_pack<p_alphabet, p_unit>(ranks);
        auto letters = letters_from_ranks<p_alphabet>(ranks);
        const auto expected_text = text_from_ranks<p_alphabet>(ranks);
        const std::uint16_t expected_padding =
            length == 0u || length % letters_per_unit == 0u
                ? 0u
                : static_cast<std::uint16_t>(letters_per_unit - length % letters_per_unit);

        compact_sequence_range<instance> directly_packed{letters};
        CAPTURE(profile, length);
        require_unit_bytes_equal<p_unit>(directly_packed.packed_units, expected);
        REQUIRE(directly_packed.padding() == expected_padding);
        REQUIRE(directly_packed.bp_size() == length);
        REQUIRE(directly_packed.to_string() == expected_text);

        const auto scalar = bioseq::simd::active_target::encode_sequence_scalar<instance>(
            expected_text.data(), expected_text.size());
        const auto simd = bioseq::simd::active_target::encode_sequence_simd<instance>(
            expected_text.data(), expected_text.size());
        require_unit_bytes_equal<p_unit>(scalar.packed_units, expected);
        require_unit_bytes_equal<p_unit>(simd.packed_units, expected);
        REQUIRE(scalar.padding() == expected_padding);
        REQUIRE(simd.padding() == expected_padding);

        require_stdc_io_oracle<p_alphabet, p_unit>(
            ranks, expected, expected_padding, expected_text);
        require_admitted_oracle<p_alphabet, p_unit>(ranks, expected, expected_padding);

        for (std::size_t split = 0u; split <= length; ++split) {
            CAPTURE(profile, length, split);
            const auto input = std::span<const std::uint8_t>{ranks};

            std::vector<p_unit> grown;
            grow_writer growing{grow_sink{grown}};
            REQUIRE(growing.append_ranks(input.first(split)) ==
                    bioseq::sequences::rank_pack_status::ok);
            REQUIRE(growing.append_ranks(input.subspan(split)) ==
                    bioseq::sequences::rank_pack_status::ok);
            const auto grow_result = growing.finish(length, expected.size());
            REQUIRE(grow_result.status == bioseq::sequences::rank_pack_status::ok);
            REQUIRE(grow_result.written_units == expected.size());
            REQUIRE(grow_result.padding_letters == expected_padding);
            require_unit_bytes_equal<p_unit>(grown, expected);

            std::vector<p_unit> fixed(expected.size());
            fixed_writer fixed_output{fixed_sink{fixed}};
            REQUIRE(fixed_output.append_ranks(input.first(split)) ==
                    bioseq::sequences::rank_pack_status::ok);
            REQUIRE(fixed_output.append_ranks(input.subspan(split)) ==
                    bioseq::sequences::rank_pack_status::ok);
            const auto fixed_result = fixed_output.finish(length, expected.size());
            REQUIRE(fixed_result.status == bioseq::sequences::rank_pack_status::ok);
            REQUIRE(fixed_result.written_units == expected.size());
            REQUIRE(fixed_result.padding_letters == expected_padding);
            require_unit_bytes_equal<p_unit>(fixed, expected);

            std::vector<p_unit> streamed;
            bioseq::simd::active_target::encode_state<p_unit, bits, letters_per_unit>
                stream_state{};
            stream_state.output_storage = &streamed;
            stream_state.reset();
            bioseq::simd::active_target::encode_body_chunk<
                p_alphabet,
                bioseq::simd::active_target::unknown_byte_policy::map_to_default,
                p_unit,
                bits,
                letters_per_unit>(
                stream_state, reinterpret_cast<const std::uint8_t*>(expected_text.data()), split);
            bioseq::simd::active_target::encode_body_chunk<
                p_alphabet,
                bioseq::simd::active_target::unknown_byte_policy::map_to_default,
                p_unit,
                bits,
                letters_per_unit>(
                stream_state,
                reinterpret_cast<const std::uint8_t*>(expected_text.data() + split),
                expected_text.size() - split);
            const auto [stream_units, stream_padding] =
                bioseq::simd::active_target::finalize_encode_state<p_unit, bits, letters_per_unit>(
                    stream_state);
            REQUIRE(stream_units == expected.size());
            REQUIRE(stream_padding == expected_padding);
            require_unit_bytes_equal<p_unit>(streamed, expected);
        }
    }

    require_invalid_and_capacity_matrix<p_alphabet, p_unit>(profile);
}

} // namespace

TEST_CASE("rank packing covers all configured profiles independently of PostgreSQL",
          "[component:bioseq][layer:unit][feature:sequence][resource:highway]") {
    // Always test DNA4 in 64-bit words. The expanded build also checks the
    // other alphabets and all four word widths.
    require_exhaustive_profile<seq::dna4, std::uint64_t>("dna4/u64");

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
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
