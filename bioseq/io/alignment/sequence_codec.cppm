/** @file Bounded native and synthetic sequence consumers for SAM/BAM records. */
export module bioseq.io.alignment.sequence_codec;

import std;
import bioseq.common.seqan_adapter;
import bioseq.common.checked_arithmetic;
import bioseq.sequences;
import bioseq.sequences.translation;
import bioseq.sequences.simd;
import bioseq.storage.lazy_sequence;
import bioseq.simd.utils;
import bioseq.highway.dispatch_contract;

export namespace bioseq::io::alignment {

/** At most one nt16 symbol awaits a partner across byte-output windows. */
struct bam_pair_state {
    std::uint8_t high{};
    bool pending{};
};

/** Exact progress returned by a bounded nibble-packing operation. */
struct bam_pair_result {
    std::size_t bytes_written{};
    bam_pair_state next;
};

/** Select the existing generated target once for a stream of bounded tiles. */
[[nodiscard]] inline bioseq::simd::sequence_rank_transform_kernel sequence_codec_kernel() {
    const auto plan = simd_dispatch::registered_highway_dispatch_plan();
    if (plan.planned_target == 0) return nullptr;
    const auto& binding = simd_dispatch::registered_highway_target_binding(plan.planned_target);
    return binding.kernels == nullptr ? nullptr : binding.kernels->common.transform_sequence_rank_chunk;
}

/** Validate numeric BAM qualities in the selected SIMD target. */
[[nodiscard]] inline bool validate_bam_quality(std::span<const std::uint8_t> bytes,
        bool missing, bioseq::simd::sequence_rank_transform_kernel transform = sequence_codec_kernel()) {
    if (transform == nullptr) return false;
    return transform(missing ? bioseq::simd::sequence_rank_transform::validate_missing_quality
                             : bioseq::simd::sequence_rank_transform::validate_phred,
        bytes, 0u, {}, {});
}

/** The canonical nt16 spelling defines SAM/BAM nucleotide wire ranks. */
inline constexpr std::string_view bam_nucleotide_symbols = "=ACMGRSVTWYHKDBN";

/** BAM uses thymine's nt16 code for uracil; the preserved alphabet restores RNA ranks. */
[[nodiscard]] constexpr char bam_nucleotide_character(char character) noexcept {
    return character == 'U' ? 'T' : character;
}

/** Map exact alphabet symbols; the fallback value cannot be a native rank. */
template<seqan3::nucleotide_alphabet Alphabet>
[[nodiscard]] consteval auto bam_to_native_rank_table() {
    std::array<std::uint32_t, 16> table{};
    table.fill(255u);
    for (std::size_t code = 1u; code < table.size(); ++code) {
        const auto symbol = seqan3::assign_char_to(bam_nucleotide_symbols[code], Alphabet{});
        if (bam_nucleotide_character(seqan3::to_char(symbol)) == bam_nucleotide_symbols[code])
            table[code] = static_cast<std::uint32_t>(seqan3::to_rank(symbol));
    }
    return table;
}

/** Decode a checked BAM byte tile directly into a caller-owned native-rank tile. */
template<seqan3::nucleotide_alphabet Alphabet>
[[nodiscard]] inline std::expected<std::size_t, std::string> decode_bam_rank_tile(
        std::span<const std::uint8_t> packed, std::span<std::uint8_t> output,
        bioseq::simd::sequence_rank_transform_kernel transform = sequence_codec_kernel()) {
    constexpr auto table = bam_to_native_rank_table<Alphabet>();
    if (transform == nullptr || !transform(bioseq::simd::sequence_rank_transform::unpack_nibbles,
            packed, seqan3::alphabet_size<Alphabet>, table, output))
        return std::unexpected(std::string{"BAM sequence has an invalid rank or tile geometry"});
    return output.size();
}

/** Expand raw BAM nt16 symbols, retaining code zero for reference-dependent '=' resolution. */
[[nodiscard]] inline std::expected<std::size_t, std::string> decode_bam_wire_tile(
        std::span<const std::uint8_t> packed, std::span<std::uint8_t> output,
        bioseq::simd::sequence_rank_transform_kernel transform = sequence_codec_kernel()) {
    constexpr auto identity = [] {
        std::array<std::uint32_t, 16> table{};
        for (std::size_t code = 0u; code < table.size(); ++code) table[code] = static_cast<std::uint32_t>(code);
        return table;
    }();
    if (transform == nullptr || !transform(bioseq::simd::sequence_rank_transform::unpack_nibbles,
            packed, identity.size(), identity, output))
        return std::unexpected(std::string{"BAM raw sequence tile geometry is invalid"});
    return output.size();
}

/** Map native sequence ranks to letters or nt16, optionally complementing each symbol. */
template<sequence_alphabet Alphabet, bool Complement, bool Packed>
[[nodiscard]] consteval auto native_wire_table() {
    constexpr auto count = Packed ? 16u : seqan3::alphabet_size<Alphabet>;
    std::array<std::uint32_t, count> table{};
    table.fill(255u);
    for (std::size_t rank = 0u; rank < seqan3::alphabet_size<Alphabet>; ++rank) {
        auto symbol = seqan3::assign_rank_to(static_cast<seqan3::alphabet_rank_t<Alphabet>>(rank), Alphabet{});
        if constexpr (Complement) {
            static_assert(seqan3::nucleotide_alphabet<Alphabet>);
            symbol = seqan3::complement(symbol);
        }
        const auto character = seqan3::to_char(symbol);
        if constexpr (Packed) {
            static_assert(seqan3::nucleotide_alphabet<Alphabet> && seqan3::alphabet_size<Alphabet> <= 16u);
            const auto found = bam_nucleotide_symbols.find(bam_nucleotide_character(character));
            if (found != std::string_view::npos) table[rank] = static_cast<std::uint32_t>(found);
        } else table[rank] = static_cast<std::uint8_t>(character);
    }
    return table;
}

/** Pack one native tile without padding an odd nonfinal boundary. */
[[nodiscard]] inline std::expected<bam_pair_result, std::string> pack_bam_rank_tile(
        std::span<const std::uint8_t> ranks, std::size_t cardinality,
        std::span<const std::uint32_t> table, bam_pair_state state,
        bool final_tile, std::span<std::uint8_t> output,
        bioseq::simd::sequence_rank_transform_kernel transform) {
    if (transform == nullptr || cardinality == 0u || cardinality > 16u || table.size() != 16u
        || (state.pending && state.high > 15u))
        return std::unexpected(std::string{"invalid BAM rank tile contract"});
    const auto pairs = ranks.size() / 2u;
    const auto remaining = ranks.size() % 2u + static_cast<std::size_t>(state.pending);
    const auto needed = pairs + remaining / 2u + static_cast<std::size_t>(final_tile && remaining % 2u != 0u);
    if (output.size() < needed)
        return std::unexpected(std::string{"BAM rank tile output capacity is insufficient"});
    std::size_t written{};
    if (state.pending && !ranks.empty()) {
        if (ranks.front() >= cardinality || table[ranks.front()] > 15u)
            return std::unexpected(std::string{"BAM source rank exceeds its alphabet"});
        output[written++] = static_cast<std::uint8_t>((state.high << 4u) | table[ranks.front()]);
        ranks = ranks.subspan(1u);
        state.pending = false;
    }
    const auto paired_symbols = ranks.size() - ranks.size() % 2u;
    if (paired_symbols != 0u && !transform(bioseq::simd::sequence_rank_transform::pack_nibbles,
            ranks.first(paired_symbols), cardinality, table, output.subspan(written, paired_symbols / 2u)))
        return std::unexpected(std::string{"BAM source rank exceeds its alphabet"});
    written += paired_symbols / 2u;
    if (paired_symbols != ranks.size()) {
        const auto rank = ranks.back();
        if (rank >= cardinality || table[rank] > 15u)
            return std::unexpected(std::string{"BAM source rank exceeds its alphabet"});
        state = {static_cast<std::uint8_t>(table[rank]), true};
    }
    if (final_tile && state.pending) {
        output[written++] = static_cast<std::uint8_t>(state.high << 4u);
        state.pending = false;
    }
    return bam_pair_result{written, state};
}

/** Stream native sequence letters or BAM nibbles from bounded original-rank windows. */
template<sequence_alphabet Alphabet, bool Reverse, bool Packed, class Sink>
[[nodiscard]] inline std::expected<std::size_t, std::string> emit_native_sequence(
        Sink& sink, const storage::borrowed_rank_window_source& source,
        bioseq::simd::sequence_rank_transform_kernel transform = sequence_codec_kernel()) {
    constexpr auto table = native_wire_table<Alphabet, Reverse && seqan3::nucleotide_alphabet<Alphabet>, Packed>();
    constexpr auto capacity = bioseq::simd::sequence_rank_io_chunk_capacity;
    if (transform == nullptr || source.alphabet_code != supported_storage_alphabets::template index_of<Alphabet>())
        return std::unexpected(std::string{"sequence source alphabet or SIMD kernel is unavailable"});
    std::array<std::uint8_t, capacity> scratch{};
    std::array<std::uint8_t, capacity> reversed{};
    std::array<std::uint8_t, capacity> encoded{};
    if constexpr (!Reverse && !Packed) {
        using configuration = sequence_custom_alphabet<Alphabet, std::uint64_t>;
        using traits = typename compact_sequence_range<configuration>::traits;
        const auto units = source.symbol_count / traits::letters_per_unit
            + static_cast<std::size_t>(source.symbol_count % traits::letters_per_unit != 0u);
        const auto padding = source.symbol_count % traits::letters_per_unit == 0u ? 0u
            : traits::letters_per_unit - source.symbol_count % traits::letters_per_unit;
        if (source.resident && source.resident->storage_unit_width == sizeof(std::uint64_t)
            && source.resident->bits_per_symbol == traits::bits_per_char
            && source.resident->leading_symbol_offset == 0u
            && source.resident->trailing_padding == padding && source.resident->unit_count == units
            && source.resident->logical_length == source.symbol_count
            && source.resident->bit_order == storage::packed_symbol_order::least_significant_symbol_first
            && (source.symbol_count == 0u || source.resident->units != nullptr)
            && reinterpret_cast<std::uintptr_t>(source.resident->units) % alignof(std::uint64_t) == 0u) {
            const auto* words = static_cast<const std::uint64_t*>(source.resident->units);
            constexpr auto tile_symbols = capacity / traits::letters_per_unit * traits::letters_per_unit;
            for (std::size_t position{}; position < source.symbol_count;) {
                const auto count = std::min(tile_symbols, source.symbol_count - position);
                bioseq::simd::active_target::decode_words_to_ascii<Alphabet, std::uint64_t,
                    traits::bits_per_char, traits::letters_per_unit>(
                        words + position / traits::letters_per_unit, encoded.data(), count);
                if (const auto written = sink.append(std::span<const std::uint8_t>{encoded}.first(count)); !written)
                    return std::unexpected(written.error());
                position += count;
            }
            return static_cast<std::size_t>(source.symbol_count);
        }
    }
    bam_pair_state state;
    std::size_t consumed{}, emitted{};
    while (consumed < source.symbol_count) {
        const auto count = std::min(capacity, source.symbol_count - consumed);
        std::size_t position = consumed;
        if constexpr (Reverse) position = source.symbol_count - consumed - count;
        const auto read = source.read_checked(position, count, scratch);
        if (!read) return std::unexpected(std::string{"sequence source window read failed"});
        auto ranks = *read;
        if constexpr (Reverse) {
            const auto destination = std::span{reversed}.first(count);
            if (!transform(bioseq::simd::sequence_rank_transform::reverse_bytes, ranks, 0u, {}, destination))
                return std::unexpected(std::string{"sequence reverse tile failed"});
            ranks = destination;
        }
        std::size_t bytes{};
        if constexpr (Packed) {
            const auto result = pack_bam_rank_tile(ranks, seqan3::alphabet_size<Alphabet>, table,
                state, count == source.symbol_count - consumed, encoded, transform);
            if (!result) return std::unexpected(result.error());
            state = result->next;
            bytes = result->bytes_written;
        } else {
            bytes = count;
            if (!transform(bioseq::simd::sequence_rank_transform::map_bytes, ranks,
                    seqan3::alphabet_size<Alphabet>, table, std::span{encoded}.first(bytes)))
                return std::unexpected(std::string{"sequence source rank exceeds its alphabet"});
        }
        const auto written = sink.append(std::span<const std::uint8_t>{encoded}.first(bytes));
        if (!written) return std::unexpected(written.error());
        emitted += bytes;
        consumed += count;
    }
    return emitted;
}

/** Frozen probability-preserving conversion of Solexa scores -5 through 62. */
inline constexpr std::array<std::uint8_t, 68> solexa_rank_to_phred{
     1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9,10,10,
    11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,
    27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,
    43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,
    59,60,61,62
};

/** Convert quality ranks to standard numeric Phred without implicit SeqAn clamping. */
template<sequences::quality_alphabet Alphabet, bool Text>
[[nodiscard]] consteval auto quality_wire_table() {
    using traits = sequences::quality_alphabet_traits<Alphabet>;
    std::array<std::uint32_t, traits::cardinality> table{};
    for (std::size_t rank = 0u; rank < table.size(); ++rank) {
        std::int16_t score{};
        if constexpr (traits::legacy) score = solexa_rank_to_phred[rank];
        else score = static_cast<std::int16_t>(rank) + traits::score_offset;
        table[rank] = score < 0 || score > 93 ? 256u
            : static_cast<std::uint32_t>(score + (Text ? 33 : 0));
    }
    return table;
}

/** Stream quality ranks, reversing read order and tripling synthetic codon quality. */
template<sequences::quality_alphabet Alphabet, bool Reverse, bool Text, class Sink>
[[nodiscard]] inline std::expected<std::size_t, std::string> emit_quality(
        Sink& sink, const storage::borrowed_rank_window_source& source, bool protein_triplets,
        bool escape_sam_single_star = false,
        bioseq::simd::sequence_rank_transform_kernel transform = sequence_codec_kernel()) {
    constexpr auto table = quality_wire_table<Alphabet, Text>();
    constexpr auto capacity = bioseq::simd::sequence_rank_io_chunk_capacity;
    if (transform == nullptr || source.alphabet_code != supported_storage_alphabets::template index_of<Alphabet>()
        || (protein_triplets && source.symbol_count > std::numeric_limits<std::size_t>::max() / 3u))
        return std::unexpected(std::string{"quality source geometry or SIMD kernel is unavailable"});
    std::array<std::uint8_t, capacity> scratch{}, reversed{}, encoded{};
    std::array<std::uint8_t, 3u * capacity> expanded{};
    std::array<std::uint32_t, 256> repeated{};
    if (protein_triplets) {
        for (std::size_t value = 0u; value < repeated.size(); ++value)
            repeated[value] = static_cast<std::uint32_t>(value | (value << 8u) | (value << 16u));
    }
    std::size_t consumed{}, emitted{};
    while (consumed < source.symbol_count) {
        const auto count = std::min(capacity, source.symbol_count - consumed);
        std::size_t position = consumed;
        if constexpr (Reverse) position = source.symbol_count - consumed - count;
        const auto read = source.read_checked(position, count, scratch);
        if (!read) return std::unexpected(std::string{"quality source window read failed"});
        auto ranks = *read;
        if constexpr (Reverse) {
            const auto destination = std::span{reversed}.first(count);
            if (!transform(bioseq::simd::sequence_rank_transform::reverse_bytes, ranks, 0u, {}, destination))
                return std::unexpected(std::string{"quality reverse tile failed"});
            ranks = destination;
        }
        auto values = std::span{encoded}.first(count);
        if (!transform(bioseq::simd::sequence_rank_transform::map_bytes, ranks, table.size(), table, values))
            return std::unexpected(std::string{"quality rank is outside standard Phred bounds"});
        if constexpr (Text) {
            if (escape_sam_single_star && !protein_triplets && source.symbol_count == 1u && values[0] == '*')
                values[0] = '+';
        }
        std::span<const std::uint8_t> bytes = values;
        if (protein_triplets) {
            auto destination = std::span{expanded}.first(3u * count);
            if (!transform(bioseq::simd::sequence_rank_transform::synthesize_triplets, values, repeated.size(), repeated, destination))
                return std::unexpected(std::string{"quality triplet tile failed"});
            bytes = destination;
        }
        const auto written = sink.append(bytes);
        if (!written) return std::unexpected(written.error());
        emitted += bytes.size();
        consumed += count;
    }
    return emitted;
}

/** Encode shared representative codons as ASCII or nt16, without a second genetic policy. */
template<seqan3::aminoacid_alphabet Alphabet, bool Reverse, bool Packed>
[[nodiscard]] consteval auto protein_wire_table() {
    std::array<std::uint32_t, seqan3::alphabet_size<Alphabet>> table{};
    for (std::size_t rank = 0u; rank < table.size(); ++rank) {
        const auto codon = sequences::select_representative_codon<Alphabet, seqan3::dna5>(rank,
            sequences::missing_codon_policy::preserve_with_ambiguous_triplet);
        if (!codon) continue;
        for (std::size_t base = 0u; base < 3u; ++base) {
            const auto index = Reverse ? 2u - base : base;
            auto symbol = seqan3::assign_rank_to((*codon)[index], seqan3::dna5{});
            if constexpr (Reverse) symbol = seqan3::complement(symbol);
            const auto character = seqan3::to_char(symbol);
            const auto wire = Packed ? static_cast<std::uint8_t>(bam_nucleotide_symbols.find(character))
                                     : static_cast<std::uint8_t>(character);
            table[rank] |= static_cast<std::uint32_t>(wire) << (8u * base);
        }
    }
    return table;
}

/** Output-only consumer for the shared protein source traversal; owns record nibble carry. */
template<seqan3::aminoacid_alphabet Alphabet, bool Packed, class Sink>
struct protein_wire_consumer {
    using codon_alphabet = seqan3::dna5;
    using result_type = std::size_t;
    Sink& sink;
    bioseq::simd::sequence_rank_transform_kernel transform{};
    bam_pair_state carry;
    std::size_t emitted_symbols{}, emitted_bytes{};
    std::string sink_error;

    /** Validate the selected target before reading even an empty source. */
    [[nodiscard]] std::expected<void, sequences::sequence_conversion_error> begin([[maybe_unused]] std::size_t count) {
        if (transform == nullptr) return std::unexpected(sequences::sequence_conversion_error{
            sequences::sequence_conversion_error_code::output_contract_violation});
        return {};
    }

    /** Synthesize one bounded source tile directly into wire-symbol scratch. */
    template<bool Reverse>
    [[nodiscard]] std::expected<void, sequences::sequence_conversion_error> consume(
            std::span<const std::uint8_t> ranks, [[maybe_unused]] std::size_t position, bool final_tile) {
        using failure = sequences::sequence_conversion_error;
        using reason = sequences::sequence_conversion_error_code;
        constexpr auto capacity = bioseq::simd::sequence_rank_io_chunk_capacity;
        constexpr auto table = protein_wire_table<Alphabet, Reverse, Packed>();
        constexpr auto identity = [] {
            std::array<std::uint32_t, 16> values{};
            for (std::size_t index = 0u; index < values.size(); ++index) values[index] = static_cast<std::uint32_t>(index);
            return values;
        }();
        if (ranks.size() > capacity) return std::unexpected(failure{reason::output_contract_violation});
        std::array<std::uint8_t, capacity> reversed{};
        std::array<std::uint8_t, 3u * capacity> wire{};
        std::array<std::uint8_t, 3u * capacity / 2u + 1u> packed{};
        if constexpr (Reverse) {
            const auto destination = std::span{reversed}.first(ranks.size());
            if (!transform(bioseq::simd::sequence_rank_transform::reverse_bytes, ranks, 0u, {}, destination))
                return std::unexpected(failure{reason::output_contract_violation});
            ranks = destination;
        }
        const auto codon_bytes = std::span{wire}.first(3u * ranks.size());
        std::span<const std::uint8_t> bytes;
        bool directly_packed{};
        if constexpr (Packed) {
            if (!carry.pending && (final_tile || ranks.size() % 2u == 0u)) {
                const auto size = codon_bytes.size() / 2u + codon_bytes.size() % 2u;
                const auto output = std::span{packed}.first(size);
                if (!transform(bioseq::simd::sequence_rank_transform::synthesize_paired_nibbles,
                        ranks, seqan3::alphabet_size<Alphabet>, table, output))
                    return std::unexpected(failure{reason::output_contract_violation});
                bytes = output;
                directly_packed = true;
            }
        }
        if (!directly_packed) {
            if (!transform(bioseq::simd::sequence_rank_transform::synthesize_triplets, ranks,
                    seqan3::alphabet_size<Alphabet>, table, codon_bytes))
                return std::unexpected(failure{reason::output_contract_violation});
            bytes = codon_bytes;
            if constexpr (Packed) {
                const auto result = pack_bam_rank_tile(codon_bytes, 16u, identity, carry, final_tile, packed, transform);
                if (!result) {
                    sink_error = result.error();
                    return std::unexpected(failure{reason::output_contract_violation});
                }
                carry = result->next;
                bytes = std::span<const std::uint8_t>{packed}.first(result->bytes_written);
            }
        }
        const auto appended = sink.append(bytes);
        if (!appended) {
            sink_error = appended.error();
            return std::unexpected(failure{reason::output_contract_violation});
        }
        emitted_symbols += codon_bytes.size();
        emitted_bytes += bytes.size();
        return {};
    }

    /** Check complete projected geometry and that no half byte escaped finalization. */
    [[nodiscard]] std::expected<result_type, sequences::sequence_conversion_error> finish(std::size_t expected_symbols) {
        const auto expected_bytes = Packed ? expected_symbols / 2u + expected_symbols % 2u : expected_symbols;
        if (emitted_symbols != expected_symbols || emitted_bytes != expected_bytes || carry.pending)
            return std::unexpected(sequences::sequence_conversion_error{
                sequences::sequence_conversion_error_code::output_contract_violation});
        return emitted_bytes;
    }
};

/** Stream synthetic protein codons through the same traversal as public backtranslation. */
template<seqan3::aminoacid_alphabet Alphabet, bool Reverse, bool Packed, class Sink>
[[nodiscard]] inline std::expected<std::size_t, std::string> emit_protein_sequence(
        Sink& sink, const storage::borrowed_rank_window_source& source,
        bioseq::simd::sequence_rank_transform_kernel transform = sequence_codec_kernel()) {
    protein_wire_consumer<Alphabet, Packed, Sink> consumer{sink, transform};
    const sequences::back_translation_options options{
        .missing = sequences::missing_codon_policy::preserve_with_ambiguous_triplet};
    const auto outcome = sequences::translation_detail::back_translate_rank_windows<Reverse, Alphabet>(
        source, 0u, source.symbol_count, options, consumer);
    if (!outcome) return std::unexpected(consumer.sink_error.empty()
        ? std::string{"protein codon conversion failed source or output validation"} : consumer.sink_error);
    return *outcome;
}

/** Invert synthetic quality replication after verifying every projected triplet agrees.
 * Input is a bounded SAM ASCII or BAM numeric tile; output retains the same byte domain. */
[[nodiscard]] inline std::expected<std::size_t, std::string> decode_quality_triplet_tile(
        std::span<const std::uint8_t> input, std::span<std::uint8_t> output,
        bioseq::simd::sequence_rank_transform_kernel transform = sequence_codec_kernel()) {
    if (transform == nullptr || !transform(bioseq::simd::sequence_rank_transform::collapse_equal_triplets,
            input, 0u, {}, output))
        return std::unexpected(std::string{"synthetic quality triplet differs or tile geometry is invalid"});
    return output.size();
}

/** Decode text through the existing alphabet character-validity table and selected gather. */
template<sequence_alphabet Alphabet>
[[nodiscard]] inline std::expected<std::size_t, std::string> decode_sam_rank_tile(
        std::span<const std::uint8_t> input, std::span<std::uint8_t> output,
        bioseq::simd::sequence_rank_transform_kernel transform = sequence_codec_kernel()) {
    constexpr auto table = [] {
        constexpr auto native = bioseq::simd::active_target::build_letter_to_rank_lut<
            Alphabet, bioseq::simd::active_target::unknown_byte_policy::reject>();
        std::array<std::uint32_t, native.size()> mapped{};
        for (std::size_t index = 0u; index < mapped.size(); ++index)
            mapped[index] = native[index] == 255u ? 256u : native[index];
        return mapped;
    }();
    if (transform == nullptr || !transform(bioseq::simd::sequence_rank_transform::map_bytes,
            input, table.size(), table, output))
        return std::unexpected(std::string{"SAM sequence character or tile geometry is invalid"});
    return output.size();
}

/** Invert numeric standard Phred bytes into an exact nonlegacy native quality alphabet. */
template<sequences::quality_alphabet Alphabet>
[[nodiscard]] inline std::expected<std::size_t, std::string> decode_bam_quality_tile(
        std::span<const std::uint8_t> input, std::span<std::uint8_t> output,
        bioseq::simd::sequence_rank_transform_kernel transform = sequence_codec_kernel()) {
    using traits = sequences::quality_alphabet_traits<Alphabet>;
    if constexpr (traits::legacy)
        return std::unexpected(std::string{"legacy quality requires its preserved original ranks"});
    constexpr auto table = [] {
        std::array<std::uint32_t, 256> mapping{};
        mapping.fill(256u);
        for (std::size_t rank = 0u; rank < traits::cardinality; ++rank) {
            const auto phred = static_cast<std::int16_t>(rank) + traits::score_offset;
            if (phred >= 0 && phred <= 93) mapping[static_cast<std::size_t>(phred)] = static_cast<std::uint32_t>(rank);
        }
        return mapping;
    }();
    if (transform == nullptr || !transform(bioseq::simd::sequence_rank_transform::map_bytes,
            input, table.size(), table, output))
        return std::unexpected(std::string{"BAM quality is outside its original alphabet"});
    return output.size();
}

} // namespace bioseq::io::alignment
