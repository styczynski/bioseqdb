/** @file Native-rank codon translation and deterministic representative synthesis. */
export module bioseq.sequences.translation;

import std;
import bioseq.common.seqan_adapter;
import bioseq.common.checked_arithmetic;
import bioseq.sequences;
import bioseq.sequences.rank_pack;
import bioseq.sequences.simd;
import bioseq.storage.lazy_sequence;
import bioseq.storage.coordinates;
import bioseq.simd.utils;

export namespace bioseq::sequences {

/** Treatment of the incomplete terminal codon after the selected frame offset. */
enum class incomplete_codon_policy : std::uint8_t { reject, drop };

/** Forward translation options; the pinned SeqAn implementation supports code 1. */
struct translation_options {
    std::int8_t frame{1};
    seqan3::genetic_code code{seqan3::genetic_code::canonical};
    incomplete_codon_policy trailing{incomplete_codon_policy::reject};
};

/** Missing canonical representatives either fail or preserve the residue via an NNN envelope. */
enum class missing_codon_policy : std::uint8_t { reject, preserve_with_ambiguous_triplet };

/** Representative codons use one deterministic source-rank right-inverse policy. */
struct back_translation_options {
    seqan3::genetic_code code{seqan3::genetic_code::canonical};
    missing_codon_policy missing{missing_codon_policy::reject};
};

/** Checked forward geometry; source offsets use the selected whole-view orientation. */
struct translation_geometry {
    std::size_t oriented_begin{};
    std::size_t output_symbols{};
};

/** Validate options and a half-open input interval before any read or allocation. */
[[nodiscard]] inline std::expected<translation_geometry, sequence_conversion_error>
make_translation_geometry(std::size_t view_size, std::size_t begin, std::size_t end,
                          const translation_options& options) {
    if (options.code != seqan3::genetic_code::canonical)
        return std::unexpected(sequence_conversion_error{
            sequence_conversion_error_code::unsupported_genetic_code});
    if (options.frame == 0 || options.frame < -3 || options.frame > 3)
        return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::invalid_frame});
    if (begin > end || end > view_size)
        return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::invalid_interval});
    if (options.trailing != incomplete_codon_policy::reject
        && options.trailing != incomplete_codon_policy::drop)
        return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::incomplete_codon});
    const auto phase = static_cast<std::size_t>((options.frame < 0 ? -options.frame : options.frame) - 1);
    const auto available = end - begin > phase ? end - begin - phase : 0u;
    const auto offset = std::min(phase, end - begin);
    const auto oriented_begin = options.frame < 0 ? view_size - end + offset : begin + offset;
    if (options.trailing == incomplete_codon_policy::reject && available % 3u != 0u) {
        const auto oriented = oriented_begin + available - available % 3u;
        return std::unexpected(sequence_conversion_error{
            .code = sequence_conversion_error_code::incomplete_codon,
            .input_position = options.frame < 0 ? view_size - 1u - oriented : oriented,
            .oriented_position = oriented});
    }
    return translation_geometry{oriented_begin, available / 3u};
}

/** Derive native destination ranks and canonical stop flags from public SeqAn APIs. */
template<seqan3::nucleotide_alphabet Source, seqan3::aminoacid_alphabet Destination>
[[nodiscard]] consteval auto translation_rank_table() {
    constexpr std::size_t source_size = seqan3::alphabet_size<Source>;
    std::array<std::uint32_t, source_size * source_size * source_size> table{};
    for (std::size_t first = 0; first < source_size; ++first)
        for (std::size_t second = 0; second < source_size; ++second)
            for (std::size_t third = 0; third < source_size; ++third) {
                const auto residue = seqan3::translate_triplet<seqan3::genetic_code::canonical>(
                    seqan3::assign_rank_to(static_cast<seqan3::alphabet_rank_t<Source>>(first), Source{}),
                    seqan3::assign_rank_to(static_cast<seqan3::alphabet_rank_t<Source>>(second), Source{}),
                    seqan3::assign_rank_to(static_cast<seqan3::alphabet_rank_t<Source>>(third), Source{}));
                const auto character = seqan3::to_char(residue);
                const auto destination = seqan3::assign_char_to(character, Destination{});
                table[(first * source_size + second) * source_size + third] =
                    static_cast<std::uint32_t>(seqan3::to_rank(destination))
                    | (static_cast<std::uint32_t>(character == '*')
                        << std::numeric_limits<std::uint8_t>::digits);
            }
    return table;
}

/** One immutable forward table per biological alphabet pair, independent of packing width. */
template<seqan3::nucleotide_alphabet Source, seqan3::aminoacid_alphabet Destination>
inline constexpr auto translation_ranks = translation_rank_table<Source, Destination>();

/** A missing representative denotes a source symbol unavailable under canonical code. */
template<seqan3::aminoacid_alphabet Source>
using reverse_translation_table = std::array<std::optional<std::array<std::uint8_t, 3>>,
                                             seqan3::alphabet_size<Source>>;

/** Select a non-stop exact-character candidate, then fewest N bases and native lexical order. */
template<seqan3::aminoacid_alphabet Source, seqan3::nucleotide_alphabet Destination>
[[nodiscard]] consteval auto make_reverse_translation_table() {
    reverse_translation_table<Source> table{};
    constexpr auto destination_size = seqan3::alphabet_size<Destination>;
    std::array<std::pair<bool, std::size_t>, seqan3::alphabet_size<Source>> scores{};
    for (std::size_t first = 0; first < destination_size; ++first)
        for (std::size_t second = 0; second < destination_size; ++second)
            for (std::size_t third = 0; third < destination_size; ++third) {
                const auto first_base = seqan3::assign_rank_to(
                    static_cast<seqan3::alphabet_rank_t<Destination>>(first), Destination{});
                const auto second_base = seqan3::assign_rank_to(
                    static_cast<seqan3::alphabet_rank_t<Destination>>(second), Destination{});
                const auto third_base = seqan3::assign_rank_to(
                    static_cast<seqan3::alphabet_rank_t<Destination>>(third), Destination{});
                const auto residue = seqan3::translate_triplet<seqan3::genetic_code::canonical>(
                    first_base, second_base, third_base);
                const auto character = seqan3::to_char(residue);
                const auto normalized = seqan3::assign_char_to(character, Source{});
                const auto source_rank = seqan3::to_rank(normalized);
                const auto source_character = seqan3::to_char(normalized);
                if (character == '*' && source_character != '*') continue;
                const auto score = std::pair{character != source_character,
                    static_cast<std::size_t>(seqan3::to_char(first_base) == 'N')
                    + static_cast<std::size_t>(seqan3::to_char(second_base) == 'N')
                    + static_cast<std::size_t>(seqan3::to_char(third_base) == 'N')};
                if (!table[source_rank] || score < scores[source_rank]) {
                    table[source_rank] = std::array{static_cast<std::uint8_t>(first),
                        static_cast<std::uint8_t>(second), static_cast<std::uint8_t>(third)};
                    scores[source_rank] = score;
                }
            }
    return table;
}

/** One immutable representative table per pair, shared by every destination word width. */
template<seqan3::aminoacid_alphabet Source, seqan3::nucleotide_alphabet Destination>
inline constexpr auto reverse_translation_ranks = make_reverse_translation_table<Source, Destination>();

/** Select the shared representative; NNN is allowed only when no representative exists. */
template<seqan3::aminoacid_alphabet Source, seqan3::nucleotide_alphabet Destination>
[[nodiscard]] constexpr std::optional<std::array<std::uint8_t, 3>> select_representative_codon(
        std::size_t rank, missing_codon_policy missing) {
    constexpr const auto& representatives = reverse_translation_ranks<Source, Destination>;
    if (rank >= representatives.size()) return std::nullopt;
    if (representatives[rank]) return representatives[rank];
    if (missing != missing_codon_policy::preserve_with_ambiguous_triplet) return std::nullopt;
    constexpr auto unknown = seqan3::assign_char_to('N', Destination{});
    if constexpr (seqan3::to_char(unknown) != 'N') return std::nullopt;
    constexpr auto rank_unknown = static_cast<std::uint8_t>(seqan3::to_rank(unknown));
    return std::array{rank_unknown, rank_unknown, rank_unknown};
}

/** Native triplet bytes derived solely from the shared selected representative policy. */
template<seqan3::aminoacid_alphabet Source, seqan3::nucleotide_alphabet Destination>
[[nodiscard]] constexpr auto make_reverse_translation_gather(missing_codon_policy missing) {
    std::array<std::uint32_t, seqan3::alphabet_size<Source>> values{};
    for (std::size_t rank = 0u; rank < values.size(); ++rank) {
        const auto codon = select_representative_codon<Source, Destination>(rank, missing);
        if (codon)
            for (std::size_t base = 0u; base < codon->size(); ++base)
                values[rank] |= static_cast<std::uint32_t>((*codon)[base]) << (8u * base);
    }
    return values;
}

/** Default inverse codebook preserves the existing rejecting public conversion policy. */
template<seqan3::aminoacid_alphabet Source, seqan3::nucleotide_alphabet Destination>
inline constexpr auto reverse_translation_gather =
    make_reverse_translation_gather<Source, Destination>(missing_codon_policy::reject);

/** A consumer receives bounded original-rank tiles and owns only destination layout. */
template<class Consumer, bool Reverse>
concept back_translation_tile_consumer = requires(Consumer& consumer,
        std::span<const std::uint8_t> ranks, std::size_t position, bool final_tile, std::size_t symbols) {
    typename Consumer::codon_alphabet;
    typename Consumer::result_type;
    requires seqan3::nucleotide_alphabet<typename Consumer::codon_alphabet>;
    { consumer.begin(symbols) } -> std::same_as<std::expected<void, sequence_conversion_error>>;
    { consumer.template consume<Reverse>(ranks, position, final_tile) }
        -> std::same_as<std::expected<void, sequence_conversion_error>>;
    { consumer.finish(symbols) }
        -> std::same_as<std::expected<typename Consumer::result_type, sequence_conversion_error>>;
};

namespace translation_detail {

/** Preserve checked pack failures without disguising internal count/rank defects. */
[[nodiscard]] inline sequence_conversion_error packing_failure(rank_pack_status status) {
    return {.code = status == rank_pack_status::capacity_overflow
            ? sequence_conversion_error_code::output_capacity
            : sequence_conversion_error_code::output_contract_violation,
        .pack_status = status};
}

/** Check the exact advertised output geometry when finalizing a caller-owned sink. */
template<sequence_conversion_sink Writer>
[[nodiscard]] auto finish(Writer& writer, std::size_t count)
    -> std::expected<rank_pack_result, sequence_conversion_error> {
    constexpr auto letters_per_unit = Writer::traits::letters_per_unit;
    auto result = writer.finish(count, count / letters_per_unit + (count % letters_per_unit != 0u));
    if (!result) return std::unexpected(packing_failure(result.status));
    return result;
}

} // namespace translation_detail
} // namespace bioseq::sequences

export namespace bioseq::sequences::translation_detail {

/** Borrow bounded native tiles, invoke the selected target, and immediately pack output. */
template<seqan3::nucleotide_alphabet Source, sequence_conversion_sink Writer>
[[nodiscard]] auto translate_rank_windows(const storage::borrowed_rank_window_source& source,
        translation_geometry geometry, bool reverse, Writer& writer,
        simd::sequence_rank_transform_kernel transform)
    -> std::expected<rank_pack_result, sequence_conversion_error> {
    using destination = typename Writer::traits::alphabet_type;
    static_assert(seqan3::aminoacid_alphabet<destination>);
    constexpr auto cardinality = seqan3::alphabet_size<Source>;
    constexpr const auto& table = translation_ranks<Source, destination>;
    constexpr auto capacity = simd::sequence_rank_io_chunk_capacity;
    static_assert(capacity <= std::numeric_limits<std::size_t>::max() / 3u);
    if (transform == nullptr) return std::unexpected(sequence_conversion_error{
        sequence_conversion_error_code::output_contract_violation});
    std::array<std::uint8_t, 3u * capacity> scratch{};
    std::array<std::uint8_t, capacity> output{};
    for (std::size_t emitted = 0; emitted < geometry.output_symbols;) {
        const auto count = std::min(capacity, geometry.output_symbols - emitted);
        const auto offset = geometry.oriented_begin + 3u * emitted;
        const auto input = source.read_checked(offset, 3u * count, scratch);
        if (!input) return std::unexpected(sequence_conversion_error{
            .code = sequence_conversion_error_code::source_read_failed,
            .input_position = reverse ? source.symbol_count - 1u - offset : offset,
            .oriented_position = offset});
        for (std::size_t index = 0; index < input->size(); ++index)
            if ((*input)[index] >= cardinality)
                return std::unexpected(sequence_conversion_error{
                    .code = sequence_conversion_error_code::invalid_source_rank,
                    .input_position = reverse ? source.symbol_count - 1u - offset - index : offset + index,
                    .oriented_position = offset + index, .source_rank = (*input)[index]});
        const auto destination_ranks = std::span<std::uint8_t>{output.data(), count};
        if (!transform(simd::sequence_rank_transform::translate_triplets, *input,
                       cardinality, table, destination_ranks))
            return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::output_contract_violation});
        const auto status = simd::active_target::append_native_ranks_chunk(
            std::span<const std::uint8_t>{destination_ranks}, writer);
        if (status != rank_pack_status::ok) return std::unexpected(packing_failure(status));
        emitted += count;
    }
    return finish(writer, geometry.output_symbols);
}

/** Read each original protein window once and feed the shared checked codon policy. */
template<bool Reverse, seqan3::aminoacid_alphabet Source, class Consumer>
    requires back_translation_tile_consumer<Consumer, Reverse>
[[nodiscard]] auto back_translate_rank_windows(const storage::borrowed_rank_window_source& source,
        std::size_t begin, std::size_t end, const back_translation_options& options, Consumer& consumer)
    -> std::expected<typename Consumer::result_type, sequence_conversion_error> {
    using destination = typename Consumer::codon_alphabet;
    if (options.code != seqan3::genetic_code::canonical)
        return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::unsupported_genetic_code});
    if (options.missing != missing_codon_policy::reject
        && options.missing != missing_codon_policy::preserve_with_ambiguous_triplet)
        return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::output_contract_violation});
    if (begin > end || end > source.symbol_count)
        return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::invalid_interval});
    if (source.orientation != storage::sequence_transform::identity
        || source.alphabet_code != supported_storage_alphabets::template index_of<Source>())
        return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::incompatible_alphabets});
    const auto projected = common::checked_mul(end - begin, std::size_t{3u});
    if (!projected)
        return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::size_overflow});
    const auto prepared = consumer.begin(*projected);
    if (!prepared) return std::unexpected(prepared.error());
    constexpr auto capacity = simd::sequence_rank_io_chunk_capacity;
    std::array<std::uint8_t, capacity> scratch{};
    std::size_t consumed{};
    while (consumed < end - begin) {
        const auto count = std::min(capacity, end - begin - consumed);
        std::size_t position = begin + consumed;
        if constexpr (Reverse) position = end - consumed - count;
        const auto ranks = source.read_checked(position, count, scratch);
        if (!ranks) return std::unexpected(sequence_conversion_error{
            .code = sequence_conversion_error_code::source_read_failed, .input_position = position});
        for (std::size_t index = 0u; index < count; ++index) {
            const auto rank = (*ranks)[index];
            if (rank >= seqan3::alphabet_size<Source>
                || !select_representative_codon<Source, destination>(rank, options.missing))
                return std::unexpected(sequence_conversion_error{
                    .code = rank >= seqan3::alphabet_size<Source>
                        ? sequence_conversion_error_code::invalid_source_rank
                        : sequence_conversion_error_code::unrepresentable_symbol,
                    .input_position = position + index, .source_rank = rank});
        }
        const auto written = consumer.template consume<Reverse>(*ranks, position, count == end - begin - consumed);
        if (!written) return std::unexpected(written.error());
        consumed += count;
    }
    return consumer.finish(*projected);
}

/** Adapt the existing packed destination to the single inverse source traversal. */
template<seqan3::aminoacid_alphabet Source, sequence_conversion_sink Writer>
struct packed_backtranslation_consumer {
    using codon_alphabet = typename Writer::traits::alphabet_type;
    using result_type = rank_pack_result;
    Writer& writer;
    simd::sequence_rank_transform_kernel transform{};
    std::span<const std::uint32_t> table;

    /** Check even empty conversions before finalizing destination storage. */
    [[nodiscard]] std::expected<void, sequence_conversion_error> begin([[maybe_unused]] std::size_t count) {
        if (transform == nullptr || table.size() != seqan3::alphabet_size<Source>)
            return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::output_contract_violation});
        return {};
    }

    /** Synthesize one bounded codon tile and append it through the existing rank packer. */
    template<bool Reverse>
    [[nodiscard]] std::expected<void, sequence_conversion_error> consume(
            std::span<const std::uint8_t> ranks, std::size_t position, [[maybe_unused]] bool final_tile) {
        static_assert(!Reverse);
        constexpr auto capacity = simd::sequence_rank_io_chunk_capacity;
        if (ranks.size() > capacity)
            return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::output_contract_violation});
        std::array<std::uint8_t, 3u * capacity> storage{};
        const auto output = std::span{storage}.first(3u * ranks.size());
        if (!transform(simd::sequence_rank_transform::synthesize_triplets, ranks,
                seqan3::alphabet_size<Source>, table, output))
            return std::unexpected(sequence_conversion_error{
                .code = sequence_conversion_error_code::output_contract_violation, .input_position = position});
        const auto status = simd::active_target::append_native_ranks_chunk(std::span<const std::uint8_t>{output}, writer);
        if (status != rank_pack_status::ok) return std::unexpected(packing_failure(status));
        return {};
    }

    /** Preserve packed word counts and padding from the existing writer. */
    [[nodiscard]] std::expected<rank_pack_result, sequence_conversion_error> finish(std::size_t count) {
        return translation_detail::finish(writer, count);
    }
};

} // namespace bioseq::sequences::translation_detail

export namespace bioseq::sequences {

/** Translate an oriented checked source using a caller-selected target kernel.
 * A null kernel returns output_contract_violation before reading or writing. */
template<seqan3::nucleotide_alphabet Source, sequence_conversion_sink Writer>
[[nodiscard]] auto translate_into(const storage::borrowed_rank_window_source& source,
        std::size_t begin, std::size_t end, const translation_options& options, Writer& writer,
        simd::sequence_rank_transform_kernel transform)
    -> std::expected<rank_pack_result, sequence_conversion_error> {
    const auto geometry = make_translation_geometry(source.symbol_count, begin, end, options);
    if (!geometry) return std::unexpected(geometry.error());
    const auto expected_orientation = options.frame < 0
        ? storage::sequence_transform::reverse_complement : storage::sequence_transform::identity;
    if (source.orientation != expected_orientation
        || source.alphabet_code != supported_storage_alphabets::template index_of<Source>())
        return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::incompatible_alphabets});
    return translation_detail::translate_rank_windows<Source>(
        source, *geometry, options.frame < 0, writer, transform);
}

/** Validate inverse options and output length, then use the caller-selected kernel.
 * A null kernel returns output_contract_violation before reading or writing. */
template<seqan3::aminoacid_alphabet Source, sequence_conversion_sink Writer>
[[nodiscard]] auto back_translate_into(const storage::borrowed_rank_window_source& source,
        std::size_t begin, std::size_t end, const back_translation_options& options, Writer& writer,
        simd::sequence_rank_transform_kernel transform)
    -> std::expected<rank_pack_result, sequence_conversion_error> {
    if (options.code != seqan3::genetic_code::canonical)
        return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::unsupported_genetic_code});
    if (begin > end || end > source.symbol_count)
        return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::invalid_interval});
    if (end - begin > std::numeric_limits<std::size_t>::max() / 3u)
        return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::size_overflow});
    if (source.orientation != storage::sequence_transform::identity
        || source.alphabet_code != supported_storage_alphabets::template index_of<Source>())
        return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::incompatible_alphabets});
    const auto table = make_reverse_translation_gather<Source, typename Writer::traits::alphabet_type>(options.missing);
    translation_detail::packed_backtranslation_consumer<Source, Writer> consumer{writer, transform, table};
    return translation_detail::back_translate_rank_windows<false, Source>(source, begin, end, options, consumer);
}

/** Convert native sequence ranks into caller-admitted packed storage.
 * Parallel work uses the existing pool only when the caller and source permit it.
 * The caller selects transform once; workers receive that function pointer by value.
 * Translation requires a nonnull kernel; same-family recoding ignores the parameter.
 * No worker acquires an input owner, allocates output, or publishes a result. */
template<sequence_alphabet Source, supports_compact_sequence_range Instance>
[[nodiscard]] auto convert_packed_into(const storage::borrowed_rank_window_source& source,
        std::size_t begin, std::size_t end, const translation_options& options,
        std::span<typename compact_sequence_range<Instance>::traits::storage_unit> output,
        bool allow_parallel, simd::sequence_rank_transform_kernel transform)
    -> std::expected<rank_pack_result, sequence_conversion_error> {
    using destination = typename compact_sequence_range<Instance>::traits::alphabet_type;
    constexpr bool forward = seqan3::nucleotide_alphabet<Source> && seqan3::aminoacid_alphabet<destination>;
    constexpr bool backward = seqan3::aminoacid_alphabet<Source> && seqan3::nucleotide_alphabet<destination>;
    static_assert((seqan3::nucleotide_alphabet<Source> || seqan3::aminoacid_alphabet<Source> || quality_alphabet<Source>)
        && (seqan3::nucleotide_alphabet<destination> || seqan3::aminoacid_alphabet<destination> || quality_alphabet<destination>));
    // The empty output path does not invoke a worker, so reject incompatible
    // quality families before handing any range to the generic output builder.
    if constexpr (quality_alphabet<Source> != quality_alphabet<destination>)
        return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::incompatible_alphabets});
    if constexpr (quality_alphabet<Source> && quality_alphabet<destination>) {
        if constexpr (quality_alphabet_traits<Source>::legacy != quality_alphabet_traits<destination>::legacy)
            return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::incompatible_alphabets});
    }
    if (begin > end || end > source.symbol_count)
        return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::invalid_interval});
    if (source.alphabet_code != supported_storage_alphabets::template index_of<Source>())
        return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::incompatible_alphabets});
    std::size_t output_count = end - begin;
    translation_geometry geometry{};
    if constexpr (forward) {
        const auto checked = make_translation_geometry(source.symbol_count, begin, end, options);
        if (!checked) return std::unexpected(checked.error());
        geometry = *checked;
        output_count = geometry.output_symbols;
    } else {
        if (source.orientation != storage::sequence_transform::identity)
            return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::incompatible_alphabets});
        if constexpr (backward) {
            if (options.code != seqan3::genetic_code::canonical)
                return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::unsupported_genetic_code});
            if (output_count > std::numeric_limits<std::size_t>::max() / 3u)
                return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::size_overflow});
            output_count *= 3u;
        }
    }
    return generate_packed_ranges<Instance>(output, output_count, backward ? 3u : 1u,
        allow_parallel && source.worker_shareable,
        [source, options, geometry, begin, transform](std::size_t offset, std::size_t count, auto& writer) {
            if constexpr (forward) {
                const auto oriented_begin = geometry.oriented_begin + 3u * offset;
                const auto task_begin = options.frame < 0
                    ? source.symbol_count - oriented_begin - 3u * count : oriented_begin;
                auto task_options = options;
                task_options.frame = options.frame < 0 ? -1 : 1;
                task_options.trailing = incomplete_codon_policy::reject;
                return translate_into<Source>(source, task_begin, task_begin + 3u * count, task_options, writer, transform);
            } else if constexpr (backward) {
                return back_translate_into<Source>(source, begin + offset / 3u,
                    begin + (offset + count) / 3u, back_translation_options{options.code}, writer, transform);
            } else {
                return simd::active_target::recode_into<Source>(source,
                    begin + offset, begin + offset + count, writer);
            }
        }, translation_detail::packing_failure);
}

/** Adapt compact native storage to the shared checked source without expanding the input. */
template<supports_compact_sequence_range R, sequence_conversion_sink Writer>
[[nodiscard]] auto translate_into(const compact_sequence_range<R>& source,
        std::size_t begin, std::size_t end, const translation_options& options, Writer& writer,
        simd::sequence_rank_transform_kernel transform)
    -> std::expected<rank_pack_result, sequence_conversion_error> {
    using alphabet = typename compact_sequence_range<R>::traits::alphabet_type;
    static_assert(seqan3::nucleotide_alphabet<alphabet>);
    if (options.frame < 0)
        return translate_into<alphabet>(storage::make_borrowed_rank_source<true>(source), begin, end, options, writer, transform);
    return translate_into<alphabet>(storage::make_borrowed_rank_source<false>(source), begin, end, options, writer, transform);
}

/** Adapt compact amino-acid storage to the same representative synthesis traversal. */
template<supports_compact_sequence_range R, sequence_conversion_sink Writer>
[[nodiscard]] auto back_translate_into(const compact_sequence_range<R>& source,
        std::size_t begin, std::size_t end, const back_translation_options& options, Writer& writer,
        simd::sequence_rank_transform_kernel transform)
    -> std::expected<rank_pack_result, sequence_conversion_error> {
    using alphabet = typename compact_sequence_range<R>::traits::alphabet_type;
    static_assert(seqan3::aminoacid_alphabet<alphabet>);
    return back_translate_into<alphabet>(storage::make_borrowed_rank_source<false>(source),
        begin, end, options, writer, transform);
}

} // namespace bioseq::sequences
