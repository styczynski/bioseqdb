/** @file Checked native SAM/BAM record framing and streaming serialization. */
module;

// Pinned Boost.JSON already accompanies jinja2cpp in the existing dependency graph.
// Its header-only fast_float implementation parses directly to binary32 without
// allocation, locale state or double rounding. Keep this internal dependency at
// the SAM numeric boundary; libc++ floating from_chars needs macOS 26 or later.
#include <boost/assert.hpp>
#include <boost/json/detail/charconv/detail/fast_float/fast_float.hpp>

export module bioseq.io.alignment.record_codec;

import std;
import bioseq.io.record_schema;
import bioseq.io.sinks;
import bioseq.common.seqan_adapter;
import bioseq.common.json_string;
import bioseq.io.simd;
import bioseq.io.alignment.sequence_codec;
import bioseq.sequences.cigar;
import bioseq.sequences;
import bioseq.storage.lazy_sequence;
import bioseq.simd.utils;

export namespace bioseq::io::alignment {

/** Read an explicitly little-endian unsigned value from checked wire storage. */
template<std::unsigned_integral Integer>
[[nodiscard]] inline Integer read_wire_unsigned(std::span<const std::uint8_t> bytes) {
    Integer value{};
    for (std::size_t index = 0; index < sizeof(Integer); ++index)
        value |= static_cast<Integer>(bytes[index]) << (8 * index);
    return value;
}

/** Read a signed little-endian value without implementation-defined narrowing. */
template<std::signed_integral Integer>
[[nodiscard]] inline Integer read_wire_signed(std::span<const std::uint8_t> bytes) {
    return std::bit_cast<Integer>(read_wire_unsigned<std::make_unsigned_t<Integer>>(bytes));
}

/** Streaming destinations own and admit their buffers; codecs never grow them. */
template<class Sink>
concept alignment_byte_sink = requires(Sink& sink, std::span<const std::uint8_t> bytes) {
    { sink.append(bytes) } -> std::same_as<std::expected<void, std::string>>;
};

/** Emit an explicitly little-endian value without alignment or host-endian assumptions. */
template<std::integral Integer, alignment_byte_sink Sink>
[[nodiscard]] inline std::expected<void, std::string> append_wire_integer(
    Sink& sink, const Integer value) {
    std::array<std::uint8_t, sizeof(Integer)> bytes{};
    using unsigned_type = std::make_unsigned_t<Integer>;
    const auto encoded = std::bit_cast<unsigned_type>(value);
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::uint8_t>(encoded >> (8 * index));
    return sink.append(bytes);
}

/** Validate one native read name; missing-name '*' is a valid one-byte name. */
[[nodiscard]] inline bool valid_read_name(std::string_view name) {
    if (name.empty() || name.size() > 254) return false;
    return std::all_of(name.begin(), name.end(), [](const char value) {
        const auto character = static_cast<unsigned char>(value);
        return character >= 33 && character <= 126 && character != '@';
    });
}

/** Validate native reference spelling, including SAM's stricter first character. */
[[nodiscard]] inline bool valid_reference_name(std::string_view name) {
    if (name.empty() || name.front() == '*' || name.front() == '=') return false;
    return std::all_of(name.begin(), name.end(), [](const char value) {
        const auto character = static_cast<unsigned char>(value);
        if (character < 33 || character > 126) return false;
        switch (character) {
            case '\\': case ',': case '"': case '\'': case '(': case ')':
            case '[': case ']': case '{': case '}': case '<': case '>': case '`':
                return false;
            default: return true;
        }
    });
}

/** Borrowed binary reference dictionary entry; length is strictly positive. */
struct bam_reference_view {
    std::string_view name;
    std::int32_t length{};
    std::size_t consumed_bytes{};
};

/** Read one dictionary entry with bounded arithmetic, retaining borrowed names. */
[[nodiscard]] inline std::expected<bam_reference_view, std::string>
parse_bam_reference(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 4)
        return std::unexpected(std::string{"BAM reference name length is truncated"});
    const auto signed_name_bytes = read_wire_signed<std::int32_t>(bytes.first(4));
    if (signed_name_bytes < 2)
        return std::unexpected(std::string{"BAM reference name is empty or invalid"});
    const auto name_bytes = static_cast<std::size_t>(signed_name_bytes);
    if (name_bytes > bytes.size() - 4 || bytes.size() - 4 - name_bytes < 4)
        return std::unexpected(std::string{"BAM reference dictionary entry is truncated"});
    const auto name = bytes.subspan(4, name_bytes);
    if (name.back() != 0
        || io::simd::scan_first_byte(0, name.data(), name.size() - 1) != name.size() - 1)
        return std::unexpected(std::string{"BAM reference name has invalid NUL termination"});
    const auto length = read_wire_signed<std::int32_t>(bytes.subspan(4 + name_bytes, 4));
    if (length <= 0)
        return std::unexpected(std::string{"BAM reference length must be positive"});
    const std::string_view name_text{reinterpret_cast<const char*>(name.data()), name_bytes - 1};
    if (!valid_reference_name(name_text))
        return std::unexpected(std::string{"BAM reference name violates SAM spelling"});
    return bam_reference_view{name_text, length, name_bytes + 8};
}

/** Checked BAM header, retaining the binary dictionary for bounded iteration. */
struct bam_header_view {
    std::string_view text;
    std::span<const std::uint8_t> dictionary;
    std::uint32_t reference_count{};
    std::size_t consumed_bytes{};
};

/** Validate a complete BAM header; a caller may retry after acquiring more bytes. */
[[nodiscard]] inline std::expected<bam_header_view, std::string>
parse_bam_header(std::span<const std::uint8_t> bytes) {
    constexpr std::array<std::uint8_t, 4> magic{'B', 'A', 'M', 1};
    if (bytes.size() < 12 || !std::equal(magic.begin(), magic.end(), bytes.begin()))
        return std::unexpected(std::string{"BAM header is truncated or has invalid magic"});
    const auto signed_text_bytes = read_wire_signed<std::int32_t>(bytes.subspan(4, 4));
    if (signed_text_bytes < 0 || static_cast<std::size_t>(signed_text_bytes) > bytes.size() - 12)
        return std::unexpected(std::string{"BAM header text length is invalid or truncated"});
    const auto text_bytes = static_cast<std::size_t>(signed_text_bytes);
    const auto signed_count = read_wire_signed<std::int32_t>(bytes.subspan(8 + text_bytes, 4));
    if (signed_count < 0)
        return std::unexpected(std::string{"BAM reference count is negative"});
    const auto count = static_cast<std::uint32_t>(signed_count);
    const auto dictionary_begin = text_bytes + 12;
    std::size_t cursor = dictionary_begin;
    for (std::uint32_t reference = 0; reference < count; ++reference) {
        const auto parsed = parse_bam_reference(bytes.subspan(cursor));
        if (!parsed) return std::unexpected(parsed.error());
        cursor += parsed->consumed_bytes;
    }
    return bam_header_view{
        {reinterpret_cast<const char*>(bytes.data() + 8), text_bytes},
        bytes.subspan(dictionary_begin, cursor - dictionary_begin), count, cursor};
}

/** One native auxiliary field plus the bytes consumed from the record tail. */
struct bam_auxiliary_view {
    auxiliary_field_view field;
    std::size_t consumed_bytes{};
};

/** Return fixed-width native auxiliary element size; zero denotes invalid type. */
[[nodiscard]] inline constexpr std::size_t auxiliary_element_bytes(const char type) {
    switch (type) {
        case 'c': case 'C': case 'A': return 1;
        case 's': case 'S': return 2;
        case 'i': case 'I': case 'f': return 4;
        default: return 0;
    }
}

/** Map a valid SAM tag to a compact duplicate-detection index. */
[[nodiscard]] inline std::optional<std::size_t> auxiliary_tag_index(
    const std::array<char, 2> tag) {
    const auto letter = [](const char character) -> std::optional<std::size_t> {
        if (character >= 'A' && character <= 'Z')
            return static_cast<std::size_t>(character - 'A');
        if (character >= 'a' && character <= 'z')
            return static_cast<std::size_t>(character - 'a' + 26);
        return {};
    };
    const auto first = letter(tag[0]);
    auto second = letter(tag[1]);
    if (!second && tag[1] >= '0' && tag[1] <= '9')
        second = static_cast<std::size_t>(tag[1] - '0' + 52);
    if (!first || !second) return {};
    return *first * 62 + *second;
}

/** Parse one typed auxiliary field without copying or allocating its payload. */
[[nodiscard]] inline std::expected<bam_auxiliary_view, std::string>
parse_bam_auxiliary(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 3)
        return std::unexpected(std::string{"BAM auxiliary field prefix is truncated"});
    auxiliary_field_view field{
        {static_cast<char>(bytes[0]), static_cast<char>(bytes[1])},
        static_cast<char>(bytes[2]), 0, {}};
    if (!auxiliary_tag_index(field.tag))
        return std::unexpected(std::string{"BAM auxiliary tag has invalid characters"});
    std::size_t prefix = 3;
    std::size_t payload_bytes = auxiliary_element_bytes(field.type);
    if (field.type == 'Z' || field.type == 'H') {
        const auto remaining = bytes.subspan(prefix);
        payload_bytes = io::simd::scan_first_byte(0, remaining.data(), remaining.size());
        if (payload_bytes == remaining.size())
            return std::unexpected(std::string{"BAM text auxiliary field lacks terminator"});
        if (field.type == 'H' && payload_bytes % 2 != 0)
            return std::unexpected(std::string{"BAM hex auxiliary field has odd length"});
        if (io::simd::scan_sam_auxiliary_prefix(remaining.data(), payload_bytes, field.type == 'H') != payload_bytes)
            return std::unexpected(std::string{"BAM text/hex auxiliary contains invalid bytes"});
        field.payload = std::as_bytes(remaining.first(payload_bytes));
        return bam_auxiliary_view{field, prefix + payload_bytes + 1};
    }
    if (field.type == 'B') {
        if (bytes.size() < 8)
            return std::unexpected(std::string{"BAM array auxiliary header is truncated"});
        field.array_element_type = static_cast<char>(bytes[3]);
        const auto element_bytes = auxiliary_element_bytes(field.array_element_type);
        if (element_bytes == 0 || field.array_element_type == 'A')
            return std::unexpected(std::string{"BAM array auxiliary element type is invalid"});
        const auto count = read_wire_unsigned<std::uint32_t>(bytes.subspan(4, 4));
        if (static_cast<std::size_t>(count) > (bytes.size() - 8) / element_bytes)
            return std::unexpected(std::string{"BAM array auxiliary length is invalid or truncated"});
        prefix = 4;
        payload_bytes = 4 + static_cast<std::size_t>(count) * element_bytes;
    } else if (payload_bytes == 0) {
        return std::unexpected(std::string{"BAM auxiliary type is invalid"});
    }
    if (payload_bytes > bytes.size() - prefix)
        return std::unexpected(std::string{"BAM auxiliary payload is truncated"});
    if (field.type == 'A' && (bytes[prefix] < 33 || bytes[prefix] > 126))
        return std::unexpected(std::string{"BAM A auxiliary field is not printable ASCII"});
    field.payload = std::as_bytes(bytes.subspan(prefix, payload_bytes));
    return bam_auxiliary_view{field, prefix + payload_bytes};
}

/** Ignore reserved SAM FLAG bits for native semantics; originals retain all 16 bits. */
[[nodiscard]] inline constexpr std::uint16_t defined_sam_flags(const std::uint16_t flags) {
    return static_cast<std::uint16_t>(flags & 0x0fffu);
}

/** Checked fixed BAM fields, independent of record storage and sequence alphabets. */
struct bam_core_fields {
    std::int32_t reference_id{-1};
    std::int32_t position{-1};
    std::uint16_t bin{};
    std::uint8_t mapping_quality{};
    std::uint16_t flags{};
    std::int32_t sequence_length{};
    std::int32_t mate_reference_id{-1};
    std::int32_t mate_position{-1};
    std::int32_t template_length{};
};

/** Borrowed native record, with CG transparently resolved to its true run words. */
struct bam_record_view {
    bam_core_fields core;
    std::string_view qname;
    std::span<const std::uint8_t> cigar_words;
    std::span<const std::uint8_t> sequence_nibbles;
    std::span<const std::uint8_t> quality;
    std::span<const std::uint8_t> auxiliary;
    std::size_t consumed_bytes{};
    bool quality_missing{};
    bool long_cigar{};
};

/** Native CIGAR geometry calculated from shared alphabet-owned operation roles. */
struct native_cigar_metrics {
    std::uint64_t query_span{};
    std::uint64_t reference_span{};
    std::uint64_t query_before_hard_clipping{};
    std::uint64_t leading_query_clip{};
    std::uint64_t trailing_query_clip{};

    /** Return half-open original-query bounds from validated native clip geometry. */
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> original_query_interval(const bool reversed) const {
        return reversed
            ? std::pair{trailing_query_clip, query_before_hard_clipping - leading_query_clip}
            : std::pair{leading_query_clip, query_before_hard_clipping - trailing_query_clip};
    }
};

/** Validate native runs and terminal clipping, without expanding run lengths. */
[[nodiscard]] inline std::expected<native_cigar_metrics, std::string>
measure_native_cigar(std::span<const std::uint8_t> words) {
    if (words.size() % 4 != 0)
        return std::unexpected(std::string{"BAM CIGAR words are truncated"});
    native_cigar_metrics metrics;
    bool saw_body = false;
    bool saw_soft = false;
    bool saw_trailing_soft = false;
    bool saw_trailing_hard = false;
    for (std::size_t offset = 0; offset < words.size(); offset += 4) {
        const auto word = read_wire_unsigned<std::uint32_t>(words.subspan(offset, 4));
        if ((word & 15) > 8)
            return std::unexpected(std::string{"BAM CIGAR opcode is not defined by SAM"});
        const auto decoded = bioseq::native_cigar_codec::decode(word);
        if (!decoded)
            return std::unexpected(std::string{"BAM CIGAR contains an invalid or zero-length run"});
        const auto role = bioseq::operation_role(decoded->operation);
        if (role == bioseq::cigar_role::hard_clip) {
            if (saw_body || saw_soft) saw_trailing_hard = true;
        } else if (role == bioseq::cigar_role::soft_clip) {
            if (saw_trailing_hard)
                return std::unexpected(std::string{"BAM soft clip follows a trailing hard clip"});
            if (saw_body) saw_trailing_soft = true;
            saw_soft = true;
        } else {
            if (saw_trailing_soft || saw_trailing_hard)
                return std::unexpected(std::string{"BAM CIGAR contains an internal clipping run"});
            saw_body = true;
        }
        if (role == bioseq::cigar_role::hard_clip || role == bioseq::cigar_role::soft_clip) {
            if (saw_body) metrics.trailing_query_clip += decoded->length;
            else metrics.leading_query_clip += decoded->length;
        }
        if (bioseq::visible_in_space(decoded->operation, bioseq::cigar_space::query))
            metrics.query_span += decoded->length;
        if (bioseq::visible_in_space(decoded->operation, bioseq::cigar_space::reference))
            metrics.reference_span += decoded->length;
        if (bioseq::visible_in_space(decoded->operation, bioseq::cigar_space::query_before_hard_clipping))
            metrics.query_before_hard_clipping += decoded->length;
    }
    return metrics;
}

/** Parse one complete BAM record and its auxiliary fields with strict bounds.
 * reference_count is the already-validated binary dictionary size. CG:B:I is
 * treated as transport only with the prescribed two-operation placeholder.
 */
[[nodiscard]] inline std::expected<bam_record_view, std::string> parse_bam_record(
    std::span<const std::uint8_t> bytes, const std::uint32_t reference_count,
    bioseq::simd::sequence_rank_transform_kernel transform = sequence_codec_kernel()) {
    if (bytes.size() < 36)
        return std::unexpected(std::string{"BAM record fixed core is truncated"});
    const auto signed_block_size = read_wire_signed<std::int32_t>(bytes.first(4));
    if (signed_block_size < 33 || static_cast<std::size_t>(signed_block_size) > bytes.size() - 4)
        return std::unexpected(std::string{"BAM record block_size is invalid or truncated"});
    const auto record = bytes.subspan(4, static_cast<std::size_t>(signed_block_size));
    const auto name_bytes = static_cast<std::size_t>(record[8]);
    const auto cigar_count = read_wire_unsigned<std::uint16_t>(record.subspan(12, 2));
    bam_record_view result;
    result.core = {
        read_wire_signed<std::int32_t>(record.subspan(0, 4)),
        read_wire_signed<std::int32_t>(record.subspan(4, 4)),
        read_wire_unsigned<std::uint16_t>(record.subspan(10, 2)), record[9],
        read_wire_unsigned<std::uint16_t>(record.subspan(14, 2)),
        read_wire_signed<std::int32_t>(record.subspan(16, 4)),
        read_wire_signed<std::int32_t>(record.subspan(20, 4)),
        read_wire_signed<std::int32_t>(record.subspan(24, 4)),
        read_wire_signed<std::int32_t>(record.subspan(28, 4))};
    const auto valid_reference = [reference_count](const std::int32_t id) {
        return id == -1 || (id >= 0 && static_cast<std::uint32_t>(id) < reference_count);
    };
    if (!valid_reference(result.core.reference_id) || !valid_reference(result.core.mate_reference_id)
        || result.core.position < -1 || result.core.mate_position < -1
        || result.core.position == std::numeric_limits<std::int32_t>::max()
        || result.core.mate_position == std::numeric_limits<std::int32_t>::max()
        || result.core.sequence_length < 0)
        return std::unexpected(std::string{"BAM fixed core contains out-of-range fields"});
    // An unavailable reference makes its coordinate uninterpreted. Retain the
    // raw field for preservation; native projection emits an absent position.
    if (name_bytes == 0 || name_bytes > record.size() - 32)
        return std::unexpected(std::string{"BAM read name length is invalid"});
    const auto name = record.subspan(32, name_bytes);
    if (name.back() != 0)
        return std::unexpected(std::string{"BAM read name lacks NUL terminator"});
    result.qname = {reinterpret_cast<const char*>(name.data()), name.size() - 1};
    if (!valid_read_name(result.qname))
        return std::unexpected(std::string{"BAM read name is invalid"});
    std::size_t cursor = 32 + name_bytes;
    const auto cigar_bytes = static_cast<std::size_t>(cigar_count) * 4;
    const auto sequence_bytes = (static_cast<std::size_t>(result.core.sequence_length) + 1) / 2;
    const auto quality_bytes = static_cast<std::size_t>(result.core.sequence_length);
    if (cigar_bytes > record.size() - cursor)
        return std::unexpected(std::string{"BAM CIGAR is truncated"});
    result.cigar_words = record.subspan(cursor, cigar_bytes);
    cursor += cigar_bytes;
    if (sequence_bytes > record.size() - cursor)
        return std::unexpected(std::string{"BAM packed sequence is truncated"});
    result.sequence_nibbles = record.subspan(cursor, sequence_bytes);
    cursor += sequence_bytes;
    if (quality_bytes > record.size() - cursor)
        return std::unexpected(std::string{"BAM quality is truncated"});
    result.quality = record.subspan(cursor, quality_bytes);
    cursor += quality_bytes;
    result.quality_missing = result.quality.empty() || result.quality.front() == 255;
    if (!validate_bam_quality(result.quality, result.quality_missing, transform))
        return std::unexpected(std::string{"BAM quality contains invalid or mixed missing scores"});
    result.auxiliary = record.subspan(cursor);
    std::bitset<52 * 62> seen_tags;
    std::optional<std::span<const std::uint8_t>> expanded_cigar;
    for (std::size_t auxiliary_offset = 0; auxiliary_offset < result.auxiliary.size();) {
        const auto parsed = parse_bam_auxiliary(result.auxiliary.subspan(auxiliary_offset));
        if (!parsed) return std::unexpected(parsed.error());
        const auto tag_index = *auxiliary_tag_index(parsed->field.tag);
        if (seen_tags.test(tag_index))
            return std::unexpected(std::string{"BAM record contains a duplicate auxiliary tag"});
        seen_tags.set(tag_index);
        if (parsed->field.tag == std::array<char, 2>{'C', 'G'}) {
            if (parsed->field.type != 'B' || parsed->field.array_element_type != 'I')
                return std::unexpected(std::string{"BAM CG transport tag must have type B:I"});
            const auto payload = parsed->field.payload;
            expanded_cigar = std::span<const std::uint8_t>{
                reinterpret_cast<const std::uint8_t*>(payload.data()) + 4, payload.size() - 4};
        }
        auxiliary_offset += parsed->consumed_bytes;
    }
    if (expanded_cigar) {
        if (cigar_count != 2 || expanded_cigar->size() / 4 <= 65535)
            return std::unexpected(std::string{"BAM CG tag does not describe a long-CIGAR placeholder"});
        const auto first = read_wire_unsigned<std::uint32_t>(result.cigar_words.first(4));
        const auto second = read_wire_unsigned<std::uint32_t>(result.cigar_words.last(4));
        if ((first & 15) != 4 || (second & 15) != 3
            || (first >> 4) != static_cast<std::uint32_t>(result.core.sequence_length))
            return std::unexpected(std::string{"BAM long-CIGAR placeholder is inconsistent"});
        const auto metrics = measure_native_cigar(*expanded_cigar);
        if (!metrics) return std::unexpected(metrics.error());
        if (metrics->reference_span != second >> 4)
            return std::unexpected(std::string{"BAM CG reference span differs from placeholder"});
        result.cigar_words = *expanded_cigar;
        result.long_cigar = true;
    }
    const auto metrics = measure_native_cigar(result.cigar_words);
    if (!metrics) return std::unexpected(metrics.error());
    if (result.core.sequence_length != 0 && !result.cigar_words.empty()
        && metrics->query_span != static_cast<std::uint32_t>(result.core.sequence_length))
        return std::unexpected(std::string{"BAM CIGAR query span does not match SEQ length"});
    if ((result.core.flags & 4u) == 0
        && (result.core.reference_id < 0 || result.core.position < 0 || result.cigar_words.empty()))
        return std::unexpected(std::string{"Mapped BAM record needs reference, position and real CIGAR"});
    result.consumed_bytes = record.size() + 4;
    return result;
}

/** Reusable native CIGAR source decodes bounded tiles from immutable wire bytes. */
struct bam_cigar_source {
    std::span<const std::uint8_t> words;

    /** Decode only the caller's next tile using shared native opcode semantics. */
    [[nodiscard]] static std::expected<bioseq::cigar_run_tile, bioseq::cigar_error> read(
        const void* context, const std::uint64_t token, std::span<bioseq::cigar_run> scratch) {
        if (context == nullptr)
            return std::unexpected(bioseq::cigar_error{bioseq::cigar_error_code::invalid_encoding});
        const auto& source = *static_cast<const bam_cigar_source*>(context);
        if (source.words.size() % 4 != 0 || token > source.words.size() / 4)
            return std::unexpected(bioseq::cigar_error{bioseq::cigar_error_code::invalid_encoding});
        const auto position = static_cast<std::size_t>(token);
        const auto count = std::min(scratch.size(), source.words.size() / 4 - position);
        for (std::size_t index = 0; index < count; ++index) {
            const auto decoded = bioseq::native_cigar_codec::decode(
                read_wire_unsigned<std::uint32_t>(source.words.subspan((position + index) * 4, 4)));
            if (!decoded) return std::unexpected(decoded.error());
            scratch[index] = *decoded;
        }
        return bioseq::cigar_run_tile{scratch.first(count), position + count,
            position + count == source.words.size() / 4};
    }

    /** Borrow this stable descriptor; it and its backing must outlive traversal. */
    [[nodiscard]] bioseq::borrowed_cigar_run_source borrow() const & noexcept {
        return {this, &read, true};
    }
    bioseq::borrowed_cigar_run_source borrow() const && = delete;
};

/** SAM's fixed columns remain borrowed from one complete admitted line. */
struct sam_record_fields {
    std::array<std::string_view, 11> fields;
    std::string_view auxiliary;
};

/** Split one complete SAM record using the existing Highway delimiter scanner. */
[[nodiscard]] inline std::expected<sam_record_fields, std::string>
parse_sam_record_fields(std::string_view line) {
    if (!line.empty() && line.back() == '\n') line.remove_suffix(1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!line.empty() && line.back() == '\t')
        return std::unexpected(std::string{"SAM record has an empty trailing column"});
    sam_record_fields result;
    std::size_t cursor{};
    for (std::size_t field = 0; field < result.fields.size(); ++field) {
        const auto tail = line.substr(cursor);
        const auto length = io::simd::scan_first_byte('\t',
            reinterpret_cast<const std::uint8_t*>(tail.data()), tail.size());
        if (length == 0 || (length == tail.size() && field + 1 < result.fields.size()))
            return std::unexpected(std::string{"SAM record has missing fixed columns"});
        result.fields[field] = tail.substr(0, length);
        cursor += length;
        if (cursor < line.size()) ++cursor;
    }
    result.auxiliary = line.substr(cursor);
    if (!valid_read_name(result.fields[0]))
        return std::unexpected(std::string{"SAM read name is invalid"});
    return result;
}

/** Parse a complete decimal integer with no partial acceptance or locale effects. */
template<std::integral Integer>
[[nodiscard]] inline std::expected<Integer, std::string> parse_sam_integer(std::string_view text) {
    Integer result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        return std::unexpected(std::string{"SAM integer is invalid or out of range"});
    return result;
}

/** Replayable CIGAR projection; clipping is expressed in original source units.
 * Protein projection scales every operation by three and maps paired =/X to M.
 * No sequence or CIGAR expansion is retained between traversals.
 */
struct cigar_wire_recipe {
    std::optional<bioseq::borrowed_cigar_run_source> source;
    std::uint64_t leading_soft_clip{};
    std::uint64_t trailing_soft_clip{};
    bool protein_triplets{};
};

/** Stream canonical projected runs as BAM words, merging adjacent operations.
 * Consumer returns expected<void,string>; all lengths split at the BAM limit.
 */
template<class Consumer>
[[nodiscard]] std::expected<void, std::string> visit_wire_cigar(
    const cigar_wire_recipe& recipe, Consumer&& consumer) {
    std::optional<bioseq::cigar_symbol> pending_operation;
    std::uint64_t pending_length{};
    const auto flush = [&pending_operation, &pending_length, &consumer]()
        -> std::expected<void, std::string> {
        if (!pending_operation) return {};
        const auto opcode = bioseq::native_cigar_codec::opcode(*pending_operation);
        if (!opcode || *opcode > 8)
            return std::unexpected(std::string{"CIGAR operation has no native SAM representation"});
        auto remaining = pending_length;
        while (remaining != 0) {
            const auto count = std::min<std::uint64_t>(remaining,
                bioseq::native_cigar_codec::maximum_count);
            const auto word = (static_cast<std::uint32_t>(count) << 4) | *opcode;
            const auto written = std::invoke(consumer, word);
            if (!written) return std::unexpected(written.error());
            remaining -= count;
        }
        pending_operation.reset();
        pending_length = 0;
        return {};
    };
    const auto append = [&recipe, &pending_operation, &pending_length, &flush](
        std::uint64_t length, bioseq::cigar_symbol operation) -> std::expected<void, std::string> {
        if (length == 0)
            return std::unexpected(std::string{"CIGAR source contains a zero-length run"});
        if (recipe.protein_triplets) {
            if (length > std::numeric_limits<std::uint64_t>::max() / 3)
                return std::unexpected(std::string{"Protein CIGAR length overflows"});
            length *= 3;
            if (bioseq::is_paired(operation))
                operation = bioseq::cigar_symbol{}.assign_char('M');
        }
        if (pending_operation && *pending_operation == operation) {
            if (length > std::numeric_limits<std::uint64_t>::max() - pending_length)
                return std::unexpected(std::string{"CIGAR coalesced length overflows"});
            pending_length += length;
        } else {
            const auto flushed = flush();
            if (!flushed) return flushed;
            pending_operation = operation;
            pending_length = length;
        }
        return {};
    };
    if (recipe.leading_soft_clip != 0) {
        const auto written = append(recipe.leading_soft_clip,
            bioseq::cigar_symbol{}.assign_char('S'));
        if (!written) return written;
    }
    if (recipe.source) {
        bioseq::borrowed_cigar_cursor cursor{*recipe.source};
        for (;;) {
            const auto run = cursor.next();
            if (!run) return std::unexpected(std::string{"Cannot read source CIGAR runs"});
            if (!*run) break;
            const auto written = append((**run).length, (**run).operation);
            if (!written) return written;
        }
    }
    if (recipe.trailing_soft_clip != 0) {
        const auto written = append(recipe.trailing_soft_clip,
            bioseq::cigar_symbol{}.assign_char('S'));
        if (!written) return written;
    }
    return flush();
}

/** Wire CIGAR dimensions needed before the fixed BAM record prefix is written. */
struct cigar_wire_measurement {
    std::uint64_t word_count{};
    native_cigar_metrics geometry;
};

/** Measure projected runs and enforce complete terminal clipping without allocation. */
[[nodiscard]] inline std::expected<cigar_wire_measurement, std::string>
measure_wire_cigar(const cigar_wire_recipe& recipe) {
    cigar_wire_measurement result;
    bool saw_body{};
    bool saw_soft{};
    bool trailing_soft{};
    bool trailing_hard{};
    const auto consumed = visit_wire_cigar(recipe,
        [&result, &saw_body, &saw_soft, &trailing_soft, &trailing_hard](
            const std::uint32_t word) -> std::expected<void, std::string> {
            const auto run = bioseq::native_cigar_codec::decode(word);
            if (!run) return std::unexpected(std::string{"Projected CIGAR is invalid"});
            const auto role = bioseq::operation_role(run->operation);
            if (role == bioseq::cigar_role::hard_clip) {
                if (saw_body || saw_soft) trailing_hard = true;
            } else if (role == bioseq::cigar_role::soft_clip) {
                if (trailing_hard)
                    return std::unexpected(std::string{"Soft clip follows a trailing hard clip"});
                if (saw_body) trailing_soft = true;
                saw_soft = true;
            } else {
                if (trailing_soft || trailing_hard)
                    return std::unexpected(std::string{"CIGAR has an internal clipping operation"});
                saw_body = true;
            }
            if (result.word_count == std::numeric_limits<std::uint64_t>::max()
                || run->length > std::numeric_limits<std::uint64_t>::max() - result.geometry.query_span
                || run->length > std::numeric_limits<std::uint64_t>::max() - result.geometry.reference_span
                || run->length > std::numeric_limits<std::uint64_t>::max() - result.geometry.query_before_hard_clipping)
                return std::unexpected(std::string{"CIGAR projected geometry overflows"});
            ++result.word_count;
            if (role == bioseq::cigar_role::hard_clip || role == bioseq::cigar_role::soft_clip) {
                if (saw_body) result.geometry.trailing_query_clip += run->length;
                else result.geometry.leading_query_clip += run->length;
            }
            if (bioseq::visible_in_space(run->operation, bioseq::cigar_space::query))
                result.geometry.query_span += run->length;
            if (bioseq::visible_in_space(run->operation, bioseq::cigar_space::reference))
                result.geometry.reference_span += run->length;
            if (bioseq::visible_in_space(run->operation, bioseq::cigar_space::query_before_hard_clipping))
                result.geometry.query_before_hard_clipping += run->length;
            return {};
        });
    if (!consumed) return std::unexpected(consumed.error());
    return result;
}

/** Derive soft clipping from declared half-open original-query coordinates. */
[[nodiscard]] inline std::expected<cigar_wire_recipe, std::string> make_cigar_wire_recipe(
    const alignment_record_view& alignment, const bool protein_triplets,
    const std::uint16_t resolved_flags) {
    cigar_wire_recipe recipe{alignment.cigar, 0, 0, protein_triplets};
    if (alignment.form == cigar_form::sam_complete || !alignment.cigar) return recipe;
    if (!alignment.query_length || !alignment.query_start || !alignment.query_end
        || *alignment.query_start > *alignment.query_end
        || *alignment.query_end > *alignment.query_length)
        return std::unexpected(std::string{"Alignment-core CIGAR needs valid query length/start/end"});
    const bool reversed = (resolved_flags & 0x10u) != 0;
    recipe.leading_soft_clip = reversed
        ? *alignment.query_length - *alignment.query_end : *alignment.query_start;
    recipe.trailing_soft_clip = reversed
        ? *alignment.query_start : *alignment.query_length - *alignment.query_end;
    return recipe;
}

/** Compute the SAM/HTSlib wire bin, including coordinates beyond BAI's index range.
 * Final narrowing matches BAM's uint16 core field; it does not imply that a BAI
 * index can represent the interval. Unplaced reads use reg2bin(-1, 0).
 */
[[nodiscard]] inline std::uint16_t bam_region_bin(
    const std::int32_t position, const std::uint64_t reference_span) {
    if (position < 0) return 4680;
    const auto begin = static_cast<std::uint64_t>(position);
    const auto end = begin + std::max<std::uint64_t>(reference_span, 1) - 1;
    if ((begin >> 14) == (end >> 14)) return static_cast<std::uint16_t>(4681 + (begin >> 14));
    if ((begin >> 17) == (end >> 17)) return static_cast<std::uint16_t>(585 + (begin >> 17));
    if ((begin >> 20) == (end >> 20)) return static_cast<std::uint16_t>(73 + (begin >> 20));
    if ((begin >> 23) == (end >> 23)) return static_cast<std::uint16_t>(9 + (begin >> 23));
    if ((begin >> 26) == (end >> 26)) return static_cast<std::uint16_t>(1 + (begin >> 26));
    return 0;
}

/** Preflight fixed prefix, encoded body size and long-CIGAR transport choice. */
struct bam_record_measurement {
    std::int32_t block_size{};
    std::uint16_t core_cigar_count{};
    bool long_cigar{};
};

/** Measure exact BAM record size; auxiliary_bytes includes canonical bq payload. */
[[nodiscard]] inline std::expected<bam_record_measurement, std::string> measure_bam_record(
    std::string_view qname, const bam_core_fields& core,
    const cigar_wire_measurement& cigar, const std::uint64_t auxiliary_bytes) {
    if (!valid_read_name(qname) || core.sequence_length < 0)
        return std::unexpected(std::string{"BAM record name or sequence length is invalid"});
    const bool long_cigar = cigar.word_count > 65535;
    if (long_cigar && (static_cast<std::uint64_t>(core.sequence_length)
            > bioseq::native_cigar_codec::maximum_count
        || cigar.geometry.reference_span > bioseq::native_cigar_codec::maximum_count))
        return std::unexpected(std::string{"BAM long-CIGAR placeholder span exceeds 28 bits"});
    const auto core_count = long_cigar ? std::uint64_t{2} : cigar.word_count;
    if (cigar.word_count > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected(std::string{"BAM CIGAR run count exceeds its array field"});
    const auto sequence_length = static_cast<std::uint64_t>(core.sequence_length);
    const auto fixed_bytes = 32 + qname.size() + 1 + core_count * 4
        + (sequence_length + 1) / 2 + sequence_length;
    const auto long_bytes = long_cigar ? 8 + cigar.word_count * 4 : 0;
    constexpr auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
    if (fixed_bytes > limit || long_bytes > limit - fixed_bytes
        || auxiliary_bytes > limit - fixed_bytes - long_bytes)
        return std::unexpected(std::string{"BAM record exceeds signed block_size capacity"});
    return bam_record_measurement{static_cast<std::int32_t>(fixed_bytes + long_bytes + auxiliary_bytes),
        static_cast<std::uint16_t>(core_count), long_cigar};
}

/** Emit BAM's fixed prefix and read name, then caller streams CIGAR/SEQ/QUAL/aux. */
template<alignment_byte_sink Sink>
[[nodiscard]] std::expected<void, std::string> emit_bam_record_prefix(
    Sink& sink, std::string_view qname, const bam_core_fields& core,
    const bam_record_measurement& measured) {
    const auto write_u32 = [&sink](const std::uint32_t value) {
        return append_wire_integer(sink, value);
    };
    const std::array<std::uint32_t, 9> fixed{
        std::bit_cast<std::uint32_t>(measured.block_size),
        std::bit_cast<std::uint32_t>(core.reference_id),
        std::bit_cast<std::uint32_t>(core.position),
        (static_cast<std::uint32_t>(core.bin) << 16)
            | (static_cast<std::uint32_t>(core.mapping_quality) << 8)
            | static_cast<std::uint32_t>(qname.size() + 1),
        (static_cast<std::uint32_t>(defined_sam_flags(core.flags)) << 16) | measured.core_cigar_count,
        std::bit_cast<std::uint32_t>(core.sequence_length),
        std::bit_cast<std::uint32_t>(core.mate_reference_id),
        std::bit_cast<std::uint32_t>(core.mate_position),
        std::bit_cast<std::uint32_t>(core.template_length)};
    for (const auto value : fixed) {
        const auto written = write_u32(value);
        if (!written) return written;
    }
    const auto name_written = sink.append(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(qname.data()), qname.size()});
    if (!name_written) return name_written;
    constexpr std::array<std::uint8_t, 1> terminator{};
    return sink.append(terminator);
}

/** Emit real core CIGAR or its required long-CIGAR transport placeholder. */
template<alignment_byte_sink Sink>
[[nodiscard]] std::expected<void, std::string> emit_bam_cigar(
    Sink& sink, const cigar_wire_recipe& recipe, const cigar_wire_measurement& cigar,
    const bam_core_fields& core, const bool long_cigar) {
    if (long_cigar) {
        const auto soft = append_wire_integer(sink,
            (static_cast<std::uint32_t>(core.sequence_length) << 4) | 4u);
        if (!soft) return soft;
        return append_wire_integer(sink,
            (static_cast<std::uint32_t>(cigar.geometry.reference_span) << 4) | 3u);
    }
    return visit_wire_cigar(recipe, [&sink](const std::uint32_t word) {
        return append_wire_integer(sink, word);
    });
}

/** Emit the CG transport tag after ordinary aux fields for a measured long CIGAR. */
template<alignment_byte_sink Sink>
[[nodiscard]] std::expected<void, std::string> emit_bam_long_cigar(
    Sink& sink, const cigar_wire_recipe& recipe, const cigar_wire_measurement& cigar) {
    constexpr std::array<std::uint8_t, 4> tag{'C', 'G', 'B', 'I'};
    const auto prefix = sink.append(tag);
    if (!prefix) return prefix;
    const auto count = append_wire_integer(sink, static_cast<std::uint32_t>(cigar.word_count));
    if (!count) return count;
    return visit_wire_cigar(recipe, [&sink](const std::uint32_t word) {
        return append_wire_integer(sink, word);
    });
}

/** Emit one already checked native auxiliary value without retaining a copy. */
template<alignment_byte_sink Sink>
[[nodiscard]] std::expected<void, std::string> emit_bam_auxiliary(
    Sink& sink, const auxiliary_field_view& field) {
    const std::array<std::uint8_t, 3> tag{
        static_cast<std::uint8_t>(field.tag[0]), static_cast<std::uint8_t>(field.tag[1]),
        static_cast<std::uint8_t>(field.type)};
    const auto prefix = sink.append(tag);
    if (!prefix) return prefix;
    if (field.type == 'B') {
        const std::array<std::uint8_t, 1> subtype{static_cast<std::uint8_t>(field.array_element_type)};
        const auto written = sink.append(subtype);
        if (!written) return written;
    }
    const auto payload = sink.append(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(field.payload.data()), field.payload.size()});
    if (!payload) return payload;
    if (field.type == 'Z' || field.type == 'H') {
        constexpr std::array<std::uint8_t, 1> terminator{};
        return sink.append(terminator);
    }
    return {};
}

/** Append a decimal number to a bounded sink using locale-independent conversion. */
template<std::integral Integer, alignment_byte_sink Sink>
[[nodiscard]] std::expected<void, std::string> append_sam_integer(Sink& sink, Integer value) {
    std::array<char, std::numeric_limits<Integer>::digits10 + 3> text{};
    const auto converted = std::to_chars(text.data(), text.data() + text.size(), value);
    if (converted.ec != std::errc{})
        return std::unexpected(std::string{"SAM integer formatting failed"});
    return sink.append(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()),
        static_cast<std::size_t>(converted.ptr - text.data())});
}

/** Emit native CIGAR text, including missing '*' for an empty run source. */
template<alignment_byte_sink Sink>
[[nodiscard]] std::expected<void, std::string> emit_sam_cigar(
    Sink& sink, const cigar_wire_recipe& recipe, const cigar_wire_measurement& measured) {
    if (measured.word_count == 0) {
        constexpr std::array<std::uint8_t, 1> missing{'*'};
        return sink.append(missing);
    }
    return visit_wire_cigar(recipe, [&sink](const std::uint32_t word)
        -> std::expected<void, std::string> {
        const auto run = bioseq::native_cigar_codec::decode(word);
        if (!run) return std::unexpected(std::string{"Invalid projected SAM CIGAR"});
        const auto count = append_sam_integer(sink, run->length);
        if (!count) return count;
        const std::array<std::uint8_t, 1> operation{
            static_cast<std::uint8_t>(seqan3::to_char(run->operation))};
        return sink.append(operation);
    });
}

/** Borrowed final dictionary name/length, already mapped to the emitted domain. */
struct wire_reference_view {
    std::string_view name;
    std::uint64_t length{};
};

/** Emit a BAM header from finalized text and dictionary without host-layout stores. */
template<alignment_byte_sink Sink>
[[nodiscard]] std::expected<void, std::string> emit_bam_header(
    Sink& sink, std::string_view text, std::span<const wire_reference_view> references) {
    constexpr auto signed_limit = static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
    if (text.size() > signed_limit || references.size() > signed_limit)
        return std::unexpected(std::string{"BAM header text or reference count exceeds signed32"});
    for (const auto& reference : references) {
        if (!valid_reference_name(reference.name) || reference.name.size() >= signed_limit
            || reference.length == 0 || reference.length > signed_limit)
            return std::unexpected(std::string{"BAM reference name or length is invalid"});
    }
    constexpr std::array<std::uint8_t, 4> magic{'B', 'A', 'M', 1};
    if (auto written = sink.append(magic); !written) return written;
    if (auto written = append_wire_integer(sink, static_cast<std::int32_t>(text.size()));
        !written) return written;
    if (auto written = sink.append(std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}); !written) return written;
    if (auto written = append_wire_integer(sink, static_cast<std::int32_t>(references.size()));
        !written) return written;
    constexpr std::array<std::uint8_t, 1> terminator{};
    for (const auto& reference : references) {
        if (auto written = append_wire_integer(sink,
                static_cast<std::int32_t>(reference.name.size() + 1)); !written) return written;
        if (auto written = sink.append(std::span<const std::uint8_t>{
                reinterpret_cast<const std::uint8_t*>(reference.name.data()), reference.name.size()});
            !written) return written;
        if (auto written = sink.append(terminator); !written) return written;
        if (auto written = append_wire_integer(sink, static_cast<std::int32_t>(reference.length));
            !written) return written;
    }
    return {};
}

/** Validate a logical auxiliary view before either binary or text serialization. */
[[nodiscard]] inline std::expected<void, std::string> validate_auxiliary_field(
    const auxiliary_field_view& field) {
    if (!auxiliary_tag_index(field.tag))
        return std::unexpected(std::string{"Auxiliary tag must match [A-Za-z][A-Za-z0-9]"});
    const auto bytes = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(field.payload.data()), field.payload.size()};
    if (field.type == 'Z' || field.type == 'H') {
        if (field.type == 'H' && bytes.size() % 2 != 0)
            return std::unexpected(std::string{"Hex auxiliary value has odd length"});
        if (io::simd::scan_sam_auxiliary_prefix(bytes.data(), bytes.size(), field.type == 'H') != bytes.size())
            return std::unexpected(std::string{"Text/hex auxiliary contains invalid bytes"});
        return {};
    }
    if (field.type == 'B') {
        const auto element_bytes = auxiliary_element_bytes(field.array_element_type);
        if (bytes.size() < 4 || element_bytes == 0 || field.array_element_type == 'A')
            return std::unexpected(std::string{"Auxiliary array header is invalid"});
        const auto count = read_wire_unsigned<std::uint32_t>(bytes.first(4));
        if (static_cast<std::uint64_t>(count) * element_bytes != bytes.size() - 4)
            return std::unexpected(std::string{"Auxiliary array count does not match its payload"});
        return {};
    }
    const auto element_bytes = auxiliary_element_bytes(field.type);
    if (element_bytes == 0 || bytes.size() != element_bytes)
        return std::unexpected(std::string{"Auxiliary scalar type/size is invalid"});
    if (field.type == 'A' && (bytes[0] < 33 || bytes[0] > 126))
        return std::unexpected(std::string{"Auxiliary character is not printable ASCII"});
    return {};
}

/** Detect values SAM cannot spell while retaining every IEEE bit for BAM/preservation. */
[[nodiscard]] inline bool auxiliary_has_nonfinite_float(const auxiliary_field_view& field) {
    auto bytes = std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(field.payload.data()), field.payload.size()};
    if (field.type == 'f') return !io::simd::all_finite_f32_le(bytes);
    if (field.type == 'B' && field.array_element_type == 'f')
        return bytes.size() < 4 || !io::simd::all_finite_f32_le(bytes.subspan(4));
    return false;
}

/** Measure checked native aux bytes; callers sum with overflow/budget admission. */
[[nodiscard]] inline std::expected<std::uint64_t, std::string> measure_bam_auxiliary(
    const auxiliary_field_view& field) {
    if (const auto valid = validate_auxiliary_field(field); !valid)
        return std::unexpected(valid.error());
    const auto prefix = field.type == 'B' ? 4u : 3u;
    const auto terminator = (field.type == 'Z' || field.type == 'H') ? 1u : 0u;
    if (field.payload.size() > std::numeric_limits<std::uint64_t>::max() - prefix - terminator)
        return std::unexpected(std::string{"BAM auxiliary byte count overflows"});
    return static_cast<std::uint64_t>(field.payload.size()) + prefix + terminator;
}

/** Emit finite IEEE float32 with enough decimal precision to roundtrip. */
template<alignment_byte_sink Sink>
[[nodiscard]] std::expected<void, std::string> append_sam_float(
    Sink& sink, const float value) {
    if (!std::isfinite(value))
        return std::unexpected(std::string{"Nonfinite float has no canonical SAM representation"});
    std::array<char, 64> text{};
    const auto converted = std::to_chars(text.data(), text.data() + text.size(), value,
        std::chars_format::general, std::numeric_limits<float>::max_digits10);
    if (converted.ec != std::errc{})
        return std::unexpected(std::string{"SAM float formatting failed"});
    return sink.append(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()),
        static_cast<std::size_t>(converted.ptr - text.data())});
}

/** Format one already checked numeric aux element according to its wire type. */
template<alignment_byte_sink Sink>
[[nodiscard]] std::expected<void, std::string> emit_sam_auxiliary_number(
    Sink& sink, const char type, std::span<const std::uint8_t> bytes) {
    switch (type) {
        case 'c': return append_sam_integer(sink, read_wire_signed<std::int8_t>(bytes));
        case 'C': return append_sam_integer(sink, read_wire_unsigned<std::uint8_t>(bytes));
        case 's': return append_sam_integer(sink, read_wire_signed<std::int16_t>(bytes));
        case 'S': return append_sam_integer(sink, read_wire_unsigned<std::uint16_t>(bytes));
        case 'i': return append_sam_integer(sink, read_wire_signed<std::int32_t>(bytes));
        case 'I': return append_sam_integer(sink, read_wire_unsigned<std::uint32_t>(bytes));
        case 'f': return append_sam_float(sink,
            std::bit_cast<float>(read_wire_unsigned<std::uint32_t>(bytes)));
        default: return std::unexpected(std::string{"SAM auxiliary numeric type is invalid"});
    }
}

/** Emit a checked SAM aux field including its leading TAB, preserving native types. */
template<alignment_byte_sink Sink>
[[nodiscard]] std::expected<void, std::string> emit_sam_auxiliary(
    Sink& sink, const auxiliary_field_view& field) {
    if (const auto valid = validate_auxiliary_field(field); !valid) return valid;
    const bool integer_type = field.type == 'c' || field.type == 'C'
        || field.type == 's' || field.type == 'S' || field.type == 'i' || field.type == 'I';
    const std::array<std::uint8_t, 6> prefix{'\t', static_cast<std::uint8_t>(field.tag[0]),
        static_cast<std::uint8_t>(field.tag[1]), ':',
        static_cast<std::uint8_t>(integer_type ? 'i' : field.type), ':'};
    if (auto written = sink.append(prefix); !written) return written;
    const auto bytes = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(field.payload.data()), field.payload.size()};
    if (field.type == 'A' || field.type == 'Z' || field.type == 'H') return sink.append(bytes);
    if (field.type == 'B') {
        const std::array<std::uint8_t, 1> subtype{static_cast<std::uint8_t>(field.array_element_type)};
        if (auto written = sink.append(subtype); !written) return written;
        constexpr std::array<std::uint8_t, 1> separator{','};
        const auto width = auxiliary_element_bytes(field.array_element_type);
        for (std::size_t cursor = 4; cursor < bytes.size(); cursor += width) {
            if (auto written = sink.append(separator); !written) return written;
            if (auto written = emit_sam_auxiliary_number(sink, field.array_element_type,
                    bytes.subspan(cursor, width)); !written) return written;
        }
        return {};
    }
    return emit_sam_auxiliary_number(sink, field.type, bytes);
}

/** Emit all SAM core columns through TLEN; caller streams SEQ, TAB, QUAL, aux, LF. */
template<alignment_byte_sink Sink>
[[nodiscard]] std::expected<void, std::string> emit_sam_record_prefix(
    Sink& sink, std::string_view qname, std::string_view reference_name,
    std::string_view mate_reference_name, const bam_core_fields& core,
    const cigar_wire_recipe& recipe, const cigar_wire_measurement& measured) {
    if (!valid_read_name(qname))
        return std::unexpected(std::string{"SAM read name is invalid"});
    if (core.reference_id < 0) reference_name = "*";
    if (core.mate_reference_id < 0) mate_reference_name = "*";
    else if (core.mate_reference_id == core.reference_id) mate_reference_name = "=";
    constexpr std::array<std::uint8_t, 1> tab{'\t'};
    const auto append_text = [&sink](std::string_view text) {
        return sink.append(std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    };
    if (auto written = append_text(qname); !written) return written;
    if (auto written = sink.append(tab); !written) return written;
    if (auto written = append_sam_integer(sink, defined_sam_flags(core.flags)); !written) return written;
    if (auto written = sink.append(tab); !written) return written;
    if (auto written = append_text(reference_name); !written) return written;
    if (auto written = sink.append(tab); !written) return written;
    if (auto written = append_sam_integer(sink, static_cast<std::int64_t>(core.position) + 1);
        !written) return written;
    if (auto written = sink.append(tab); !written) return written;
    if (auto written = append_sam_integer(sink, core.mapping_quality); !written) return written;
    if (auto written = sink.append(tab); !written) return written;
    if (auto written = emit_sam_cigar(sink, recipe, measured); !written) return written;
    if (auto written = sink.append(tab); !written) return written;
    if (auto written = append_text(mate_reference_name); !written) return written;
    if (auto written = sink.append(tab); !written) return written;
    if (auto written = append_sam_integer(sink, static_cast<std::int64_t>(core.mate_position) + 1);
        !written) return written;
    if (auto written = sink.append(tab); !written) return written;
    if (auto written = append_sam_integer(sink, core.template_length); !written) return written;
    return sink.append(tab);
}

/** Final dictionary bindings; reference lengths are original source symbols. */
struct native_reference_binding {
    std::int32_t reference_id{-1};
    std::int32_t mate_reference_id{-1};
    std::optional<std::uint64_t> reference_length;
    std::optional<std::uint64_t> mate_reference_length;
    bool reference_is_circular{};
    /** A bound mate descriptor must identify its actual projection domain. */
    bool mate_same_projection_domain{};
};

/** Fully checked native geometry, retaining replay recipes rather than CIGAR copies. */
struct native_alignment_plan {
    bam_core_fields core;
    cigar_wire_recipe cigar;
    cigar_wire_measurement cigar_size;
    /** Original mate coordinates/sequence have no truthful native projection. */
    bool mate_projection_suppressed{};
};

/** Validate original geometry and derive a complete native alignment projection.
 * Flags have already passed source classification/election. Explicit original
 * fields remain untouched for preservation; only the returned native view changes.
 */
[[nodiscard]] inline std::expected<native_alignment_plan, std::string> prepare_native_alignment(
    const alignment_record_view& alignment, const std::uint16_t resolved_flags,
    const std::optional<std::uint64_t> source_sequence_length,
    const native_reference_binding& references, const bool protein_triplets) {
    constexpr auto coordinate_limit = static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
    const std::uint64_t scale = protein_triplets ? 3 : 1;
    for (std::size_t bit = 0; bit < alignment.flag_constraints.size(); ++bit) {
        if (alignment.flag_constraints[bit]
            && *alignment.flag_constraints[bit] != ((resolved_flags & (std::uint16_t{1} << bit)) != 0))
            return std::unexpected(std::string{"Alignment FLAG contradicts an explicitly mapped boolean"});
    }
    const auto reference_length = references.reference_length
        ? references.reference_length : alignment.reference_length;
    if (references.reference_length && alignment.reference_length
        && *references.reference_length != *alignment.reference_length)
        return std::unexpected(std::string{"Alignment reference length disagrees with dictionary"});
    if (reference_length && (*reference_length == 0
        || (references.reference_id >= 0 && *reference_length > coordinate_limit / scale)))
        return std::unexpected(std::string{"Projected reference length exceeds SAM signed32 limits"});
    if (source_sequence_length && *source_sequence_length > coordinate_limit / scale)
        return std::unexpected(std::string{"Projected sequence length exceeds native signed32 limits"});
    if (alignment.target_start && references.reference_id >= 0
        && *alignment.target_start > (coordinate_limit - 1) / scale)
        return std::unexpected(std::string{"Projected target position exceeds native signed32 limits"});
    const bool mapped = (resolved_flags & 4u) == 0;
    if (mapped && (references.reference_id < 0 || !alignment.target_start || !alignment.cigar))
        return std::unexpected(std::string{"Mapped alignment needs reference, position and real CIGAR"});
    if (mapped && references.reference_id >= 0 && alignment.target_start && reference_length
        && *alignment.target_start >= *reference_length)
        return std::unexpected(std::string{"Alignment starts outside its reference"});
    const auto recipe = make_cigar_wire_recipe(alignment, protein_triplets, resolved_flags);
    if (!recipe) return std::unexpected(recipe.error());
    const auto measured = measure_wire_cigar(*recipe);
    if (!measured) return std::unexpected(measured.error());
    if (mapped && measured->word_count == 0)
        return std::unexpected(std::string{"Mapped alignment has an empty CIGAR"});
    if (source_sequence_length && measured->word_count != 0
        && measured->geometry.query_span != *source_sequence_length * scale)
        return std::unexpected(std::string{"Complete CIGAR query consumption differs from sequence length"});
    if (alignment.query_length && *alignment.query_length > std::numeric_limits<std::uint64_t>::max() / scale)
        return std::unexpected(std::string{"Projected original query length overflows"});
    if (alignment.query_length && measured->word_count != 0
        && measured->geometry.query_before_hard_clipping != *alignment.query_length * scale)
        return std::unexpected(std::string{"Complete CIGAR differs from original query length"});
    if ((alignment.query_start && alignment.query_end && *alignment.query_start > *alignment.query_end)
        || (alignment.query_length && ((alignment.query_start && *alignment.query_start > *alignment.query_length)
            || (alignment.query_end && *alignment.query_end > *alignment.query_length))))
        return std::unexpected(std::string{"Original query coordinates are outside their declared interval"});
    if (measured->word_count != 0) {
        const auto [query_start, query_end] = measured->geometry.original_query_interval((resolved_flags & 16u) != 0u);
        if ((alignment.query_start && *alignment.query_start != query_start / scale)
            || (alignment.query_end && *alignment.query_end != query_end / scale))
            return std::unexpected(std::string{"Original query coordinates disagree with complete CIGAR clipping"});
    }
    if (alignment.target_start && alignment.target_end) {
        if (*alignment.target_end < *alignment.target_start
            || *alignment.target_end - *alignment.target_start != measured->geometry.reference_span / scale)
            return std::unexpected(std::string{"Alignment target span disagrees with CIGAR"});
    }
    if (mapped && references.reference_id >= 0 && alignment.target_start && reference_length && !references.reference_is_circular
        && measured->geometry.reference_span > (*reference_length - *alignment.target_start) * scale)
        return std::unexpected(std::string{"Alignment extends beyond its linear reference"});
    native_alignment_plan plan;
    plan.cigar = *recipe;
    plan.cigar_size = *measured;
    plan.core.flags = defined_sam_flags(resolved_flags);
    plan.core.mapping_quality = alignment.mapping_quality.value_or(255);
    plan.core.reference_id = references.reference_id;
    plan.core.position = alignment.target_start && references.reference_id >= 0
        ? static_cast<std::int32_t>(*alignment.target_start * scale) : -1;
    plan.core.sequence_length = source_sequence_length
        ? static_cast<std::int32_t>(*source_sequence_length * scale) : 0;
    const bool paired = (resolved_flags & 1u) != 0;
    // Only a paired, mapped mate gives its retained coordinate mapped-reference semantics.
    const bool mate_mapped = paired && (resolved_flags & 8u) == 0;
    if (mate_mapped && references.mate_reference_id >= 0 && alignment.mate_start && references.mate_reference_length
        && *alignment.mate_start >= *references.mate_reference_length)
        return std::unexpected(std::string{"Alignment mate starts outside its reference"});
    // A found descriptor proves its domain for nucleotide and protein queries alike.
    // Absent nucleotide mate bindings keep their existing unknown-coordinate semantics.
    const bool native_mate_known = references.mate_reference_id < 0
        ? !protein_triplets : references.mate_same_projection_domain;
    plan.mate_projection_suppressed = !native_mate_known
        && (paired || references.mate_reference_id >= 0);
    if (plan.mate_projection_suppressed) {
        if (paired)
            plan.core.flags = static_cast<std::uint16_t>((plan.core.flags | 8u) & ~std::uint16_t{2u | 32u});
    } else {
        if (alignment.mate_start && references.mate_reference_id >= 0
            && *alignment.mate_start > (coordinate_limit - 1) / scale)
            return std::unexpected(std::string{"Projected mate position exceeds signed32"});
        constexpr auto signed_limit = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
        if (alignment.template_length < -signed_limit / static_cast<std::int64_t>(scale)
            || alignment.template_length > signed_limit / static_cast<std::int64_t>(scale))
            return std::unexpected(std::string{"Projected template length exceeds signed32"});
        plan.core.mate_reference_id = references.mate_reference_id;
        plan.core.mate_position = alignment.mate_start && references.mate_reference_id >= 0
            ? static_cast<std::int32_t>(*alignment.mate_start * scale) : -1;
        plan.core.template_length = static_cast<std::int32_t>(
            alignment.template_length * static_cast<std::int64_t>(scale));
    }
    if (protein_triplets && (plan.core.flags & 2u) != 0
        && (!paired || !native_mate_known || plan.core.position < 0 || plan.core.mate_position < 0))
        plan.core.flags = static_cast<std::uint16_t>(plan.core.flags & ~std::uint16_t{2});
    plan.core.bin = bam_region_bin(plan.core.position,
        mapped ? measured->geometry.reference_span : std::uint64_t{1});
    return plan;
}

/** Bounded text-CIGAR adapter; byte tokens avoid reparsing earlier windows. */
struct sam_cigar_source {
    std::string_view text;

    /** Parse at most scratch capacity runs, retaining no expanded operation stream. */
    [[nodiscard]] static std::expected<bioseq::cigar_run_tile, bioseq::cigar_error> read(
        const void* context, const std::uint64_t token, std::span<bioseq::cigar_run> scratch) {
        if (context == nullptr)
            return std::unexpected(bioseq::cigar_error{bioseq::cigar_error_code::invalid_encoding});
        const auto& source = *static_cast<const sam_cigar_source*>(context);
        if (token > source.text.size())
            return std::unexpected(bioseq::cigar_error{bioseq::cigar_error_code::invalid_encoding});
        if (source.text == "*") return bioseq::cigar_run_tile{{}, source.text.size(), true};
        auto position = static_cast<std::size_t>(token);
        std::size_t used{};
        while (position < source.text.size() && used < scratch.size()) {
            std::uint32_t length{};
            const auto parsed = std::from_chars(source.text.data() + position,
                source.text.data() + source.text.size(), length);
            if (parsed.ec != std::errc{} || parsed.ptr == source.text.data() + source.text.size()
                || length == 0 || parsed.ptr == source.text.data() + position)
                return std::unexpected(bioseq::cigar_error{bioseq::cigar_error_code::invalid_text, position});
            const auto character = *parsed.ptr;
            if (!seqan3::char_is_valid_for<bioseq::cigar_symbol>(character))
                return std::unexpected(bioseq::cigar_error{bioseq::cigar_error_code::invalid_operation, position});
            const auto operation = bioseq::cigar_symbol{}.assign_char(character);
            const auto opcode = bioseq::native_cigar_codec::opcode(operation);
            if (!opcode || *opcode > 8)
                return std::unexpected(bioseq::cigar_error{bioseq::cigar_error_code::invalid_operation, position});
            scratch[used++] = {length, operation};
            position = static_cast<std::size_t>(parsed.ptr + 1 - source.text.data());
        }
        return bioseq::cigar_run_tile{scratch.first(used), position, position == source.text.size()};
    }

    /** Borrow a stable descriptor whose text remains alive throughout traversal. */
    [[nodiscard]] bioseq::borrowed_cigar_run_source borrow() const & noexcept {
        return {this, &read, true};
    }
    bioseq::borrowed_cigar_run_source borrow() const && = delete;
};

/** Native SAM core plus borrowed text body; dictionary resolution follows parsing. */
struct sam_record_view {
    std::string_view qname;
    std::uint16_t flags{};
    std::string_view reference_name;
    std::optional<std::uint64_t> position;
    std::uint8_t mapping_quality{};
    std::string_view cigar;
    std::string_view mate_reference_name;
    std::optional<std::uint64_t> mate_position;
    std::int32_t template_length{};
    std::optional<std::string_view> sequence;
    std::optional<std::string_view> quality;
    std::string_view auxiliary;
    cigar_wire_measurement cigar_size;
};

/** Validate SAM fixed fields and complete CIGAR, retaining original forward storage.
 * Sequence/quality alphabet conversion uses the shared SIMD consumer afterwards.
 */
[[nodiscard]] inline std::expected<sam_record_view, std::string>
parse_sam_record(std::string_view line) {
    const auto split = parse_sam_record_fields(line);
    if (!split) return std::unexpected(split.error());
    const auto& fields = split->fields;
    const auto flags = parse_sam_integer<std::uint16_t>(fields[1]);
    const auto position = parse_sam_integer<std::uint32_t>(fields[3]);
    const auto mapq = parse_sam_integer<std::uint16_t>(fields[4]);
    const auto mate_position = parse_sam_integer<std::uint32_t>(fields[7]);
    const auto template_length = parse_sam_integer<std::int32_t>(fields[8]);
    constexpr auto coordinate_limit = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
    if (!flags || !position || !mapq || !mate_position || !template_length
        || *mapq > 255 || *position > coordinate_limit
        || *mate_position > coordinate_limit || *template_length == std::numeric_limits<std::int32_t>::min())
        return std::unexpected(std::string{"SAM fixed numeric field is invalid or out of range"});
    if ((fields[2] != "*" && !valid_reference_name(fields[2]))
        || (fields[6] != "*" && fields[6] != "=" && !valid_reference_name(fields[6]))
        || (fields[6] == "=" && fields[2] == "*"))
        return std::unexpected(std::string{"SAM reference name or coordinate is inconsistent"});
    sam_record_view result;
    result.qname = fields[0];
    result.flags = *flags;
    result.reference_name = fields[2];
    if (*position != 0) result.position = *position - 1;
    result.mapping_quality = static_cast<std::uint8_t>(*mapq);
    result.cigar = fields[5];
    result.mate_reference_name = fields[6];
    if (*mate_position != 0) result.mate_position = *mate_position - 1;
    result.template_length = *template_length;
    if (fields[9] != "*") result.sequence = fields[9];
    if (fields[10] != "*") result.quality = fields[10];
    result.auxiliary = split->auxiliary;
    if (result.quality && (!result.sequence || result.sequence->size() != result.quality->size()))
        return std::unexpected(std::string{"SAM SEQ and QUAL lengths disagree"});
    sam_cigar_source source{result.cigar};
    const auto measured = measure_wire_cigar({source.borrow(), 0, 0, false});
    if (!measured) return std::unexpected(measured.error());
    result.cigar_size = *measured;
    if (result.sequence && measured->word_count != 0
        && measured->geometry.query_span != result.sequence->size())
        return std::unexpected(std::string{"SAM CIGAR query consumption differs from SEQ length"});
    if ((result.flags & 4u) == 0
        && (result.reference_name == "*" || !result.position || measured->word_count == 0))
        return std::unexpected(std::string{"Mapped SAM record needs reference, position and real CIGAR"});
    return result;
}

/** Store one explicit little-endian scalar into prechecked caller storage. */
template<std::integral Integer>
inline void store_wire_integer(std::span<std::uint8_t> output, const Integer value) {
    const auto encoded = std::bit_cast<std::make_unsigned_t<Integer>>(value);
    for (std::size_t index = 0; index < sizeof(Integer); ++index)
        output[index] = static_cast<std::uint8_t>(encoded >> (8 * index));
}

/** Parse one numeric SAM element into its declared binary type with strict range checks. */
[[nodiscard]] inline std::expected<void, std::string> parse_sam_auxiliary_number(
    const char type, std::string_view text, std::span<std::uint8_t> output) {
    if (text.starts_with("+-"))
        return std::unexpected(std::string{"SAM numeric auxiliary has multiple signs"});
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    const auto width = auxiliary_element_bytes(type);
    if (width == 0 || type == 'A' || output.size() < width)
        return std::unexpected(std::string{"SAM numeric auxiliary destination is invalid"});
    if (type == 'f') {
        float value{};
        const auto parsed = boost::json::detail::charconv::detail::fast_float::from_chars(
            text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value))
            return std::unexpected(std::string{"SAM float auxiliary value is invalid"});
        store_wire_integer(output, std::bit_cast<std::uint32_t>(value));
        return {};
    }
    const auto parsed = parse_sam_integer<std::int64_t>(text);
    if (!parsed) return std::unexpected(parsed.error());
    const bool is_signed = type == 'c' || type == 's' || type == 'i';
    const auto bits = width * 8;
    const auto maximum = is_signed ? (std::int64_t{1} << (bits - 1)) - 1
        : (std::int64_t{1} << bits) - 1;
    const auto minimum = is_signed ? -(std::int64_t{1} << (bits - 1)) : 0;
    if (*parsed < minimum || *parsed > maximum)
        return std::unexpected(std::string{"SAM numeric auxiliary element overflows its subtype"});
    const auto encoded = static_cast<std::uint64_t>(*parsed);
    for (std::size_t index = 0; index < width; ++index)
        output[index] = static_cast<std::uint8_t>(encoded >> (8 * index));
    return {};
}

/** Parse a scalar SAM tag; text borrows its record and numeric values borrow bounded scratch. */
[[nodiscard]] inline std::expected<auxiliary_field_view, std::string> parse_sam_auxiliary_scalar(
    std::string_view text, std::span<std::uint8_t> scratch) {
    if (text.size() < 5 || text[2] != ':' || text[4] != ':')
        return std::unexpected(std::string{"SAM auxiliary field prefix is invalid"});
    auxiliary_field_view field{{text[0], text[1]}, text[3], 0, {}};
    if (!auxiliary_tag_index(field.tag))
        return std::unexpected(std::string{"SAM auxiliary tag is invalid"});
    const auto value = text.substr(5);
    if (field.type == 'Z' || field.type == 'H' || field.type == 'A') {
        field.payload = std::as_bytes(std::span<const char>{value.data(), value.size()});
        if (const auto valid = validate_auxiliary_field(field); !valid)
            return std::unexpected(valid.error());
        return field;
    }
    if (field.type == 'i') {
        auto integer_text = value;
        if (integer_text.starts_with("+-"))
            return std::unexpected(std::string{"SAM integer auxiliary has multiple signs"});
        if (!integer_text.empty() && integer_text.front() == '+') integer_text.remove_prefix(1);
        const auto integer = parse_sam_integer<std::int64_t>(integer_text);
        if (!integer || *integer < std::numeric_limits<std::int32_t>::min()
            || *integer > std::numeric_limits<std::uint32_t>::max() || scratch.size() < 4)
            return std::unexpected(std::string{"SAM integer auxiliary is out of range or lacks admitted storage"});
        field.type = *integer > std::numeric_limits<std::int32_t>::max() ? 'I' : 'i';
        store_wire_integer(scratch, static_cast<std::uint32_t>(*integer));
        field.payload = std::as_bytes(scratch.first(4));
        return field;
    }
    if (field.type == 'f') {
        if (const auto parsed = parse_sam_auxiliary_number('f', value, scratch); !parsed)
            return std::unexpected(parsed.error());
        field.payload = std::as_bytes(scratch.first(4));
        return field;
    }
    return std::unexpected(std::string{"SAM auxiliary scalar type is invalid"});
}

/** Stream one SAM B array into the shared admitted auxiliary owner, spilling on pressure. */
[[nodiscard]] inline std::expected<owned_auxiliary_field, std::string> parse_sam_auxiliary_array(
    std::string_view text, io_memory_credit* credit = nullptr) {
    if (text.size() < 6 || text[2] != ':' || text[3] != 'B' || text[4] != ':')
        return std::unexpected(std::string{"SAM array auxiliary prefix is invalid"});
    const std::array<char, 2> tag{text[0], text[1]};
    if (!auxiliary_tag_index(tag)) return std::unexpected(std::string{"SAM array auxiliary tag is invalid"});
    const auto value = text.substr(5);
    const auto subtype = value.front();
    const auto width = auxiliary_element_bytes(subtype);
    if (width == 0 || subtype == 'A' || (value.size() > 1 && value[1] != ','))
        return std::unexpected(std::string{"SAM B auxiliary subtype is invalid"});
    owned_auxiliary_field owner{tag, 'B', subtype, credit};
    constexpr std::array<std::byte, 4> prefix{};
    if (auto begun = owner.payload.append(prefix); !begun) return std::unexpected(begun.error());
    std::array<std::uint8_t, 4096> scratch{};
    std::size_t used{}, cursor = value.size() == 1 ? 1 : 2;
    std::uint32_t count{};
    while (cursor < value.size()) {
        const auto tail = value.substr(cursor);
        const auto length = io::simd::scan_first_byte(',', reinterpret_cast<const std::uint8_t*>(tail.data()), tail.size());
        if (count == std::numeric_limits<std::uint32_t>::max())
            return std::unexpected(std::string{"SAM B auxiliary count exceeds uint32"});
        if (scratch.size() - used < width) {
            if (auto appended = owner.payload.append(std::as_bytes(std::span{scratch}.first(used))); !appended)
                return std::unexpected(appended.error());
            used = 0;
        }
        if (auto parsed = parse_sam_auxiliary_number(subtype, tail.substr(0, length), std::span{scratch}.subspan(used, width)); !parsed)
            return std::unexpected(parsed.error());
        used += width;
        ++count;
        cursor += length;
        if (cursor < value.size()) {
            ++cursor;
            if (cursor == value.size()) return std::unexpected(std::string{"SAM B auxiliary has a trailing comma"});
        }
    }
    if (value.size() > 1 && count == 0) return std::unexpected(std::string{"SAM B auxiliary has an empty array element"});
    if (auto appended = owner.payload.append(std::as_bytes(std::span{scratch}.first(used))); !appended)
        return std::unexpected(appended.error());
    if (auto finished = owner.finish(count); !finished) return std::unexpected(finished.error());
    return owner;
}

/** Compare a streamed projection with retained native bytes using existing SIMD.
 * This sink is reusable for SEQ, QUAL, and other canonical byte projections.
 */
class native_projection_comparison_sink {
    std::span<const std::uint8_t> expected_;
    std::size_t position_{};
    std::uint8_t final_byte_mask_;
public:
    /** Borrow the native field; only its final byte may contain explicitly unused bits. */
    explicit native_projection_comparison_sink(std::span<const std::uint8_t> expected,
        const std::uint8_t final_byte_mask = 0xffu)
        : expected_(expected), final_byte_mask_(final_byte_mask) {}

    /** Compare one bounded chunk; a mismatch never exposes restored original data. */
    [[nodiscard]] std::expected<void, std::string> append(std::span<const std::uint8_t> bytes) {
        if (bytes.size() > expected_.size() - position_)
            return std::unexpected(std::string{"Preserved projection exceeds native field length"});
        if (bytes.empty()) return {};
        const bool reaches_final_byte = bytes.size() == expected_.size() - position_;
        const auto exact_bytes = bytes.size() - static_cast<std::size_t>(reaches_final_byte && final_byte_mask_ != 0xffu);
        if (bioseq::simd::first_unequal_sequence_rank(bytes.data(), expected_.data() + position_,
                exact_bytes) != exact_bytes
            || (exact_bytes != bytes.size()
                && ((bytes.back() ^ expected_.back()) & final_byte_mask_) != 0u))
            return std::unexpected(std::string{"Preserved projection differs from native field"});
        position_ += bytes.size();
        return {};
    }

    /** Confirm the projected stream consumed the complete native field. */
    [[nodiscard]] std::expected<void, std::string> finish() const {
        if (position_ != expected_.size())
            return std::unexpected(std::string{"Preserved projection is shorter than native field"});
        return {};
    }
};

/** Compare projected and native CIGAR runs modulo adjacent-run splitting. */
[[nodiscard]] inline std::expected<void, std::string> validate_cigar_projection(
    const cigar_wire_recipe& recipe, const bioseq::borrowed_cigar_run_source& native_source) {
    bioseq::borrowed_cigar_cursor cursor{native_source};
    std::uint64_t remaining{};
    std::optional<bioseq::cigar_symbol> operation;
    const auto compared = visit_wire_cigar(recipe,
        [&cursor, &remaining, &operation](const std::uint32_t expected_word)
            -> std::expected<void, std::string> {
            std::uint64_t expected_remaining = expected_word >> 4;
            const auto expected_operation = bioseq::native_cigar_codec::decode(expected_word);
            if (!expected_operation) return std::unexpected(std::string{"Projected CIGAR word is invalid"});
            while (expected_remaining != 0) {
                if (remaining == 0) {
                    const auto next = cursor.next();
                    if (!next || !*next) return std::unexpected(std::string{"Preserved CIGAR is longer than native CIGAR"});
                    remaining = (**next).length;
                    operation = (**next).operation;
                    if (remaining == 0) return std::unexpected(std::string{"Native CIGAR contains a zero run"});
                }
                if (operation != expected_operation->operation)
                    return std::unexpected(std::string{"Preserved CIGAR operation differs from native CIGAR"});
                const auto consumed = std::min(remaining, expected_remaining);
                remaining -= consumed;
                expected_remaining -= consumed;
            }
            return {};
        });
    if (!compared) return compared;
    const auto next = cursor.next();
    if (remaining != 0 || !next || *next)
        return std::unexpected(std::string{"Preserved CIGAR is shorter than native CIGAR"});
    return {};
}

/** Borrow native BAM words through the same bounded CIGAR comparison algorithm. */
[[nodiscard]] inline std::expected<void, std::string> validate_cigar_projection(
    const cigar_wire_recipe& recipe, std::span<const std::uint8_t> native_words) {
    const bam_cigar_source source{native_words};
    return validate_cigar_projection(recipe, source.borrow());
}

/** Compare native semantics, ignoring reserved FLAG bits and derived bin/padding. */
[[nodiscard]] inline std::expected<void, std::string> validate_bam_core_projection(
    const native_alignment_plan& projected, const bam_record_view& native) {
    const auto& expected = projected.core;
    const auto& actual = native.core;
    if (expected.flags != defined_sam_flags(actual.flags) || expected.mapping_quality != actual.mapping_quality
        || expected.reference_id != actual.reference_id || expected.position != actual.position
        || expected.mate_reference_id != actual.mate_reference_id || expected.mate_position != actual.mate_position
        || expected.template_length != actual.template_length || expected.sequence_length != actual.sequence_length)
        return std::unexpected(std::string{"Preserved alignment projection differs from native BAM core"});
    return validate_cigar_projection(projected.cigar, native.cigar_words);
}

/** Normalize all legal BAM integer widths into one exact signed64 logical value. */
[[nodiscard]] inline std::optional<std::int64_t> auxiliary_integer_value(
    const auxiliary_field_view& field) {
    const auto bytes = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(field.payload.data()), field.payload.size()};
    if (bytes.size() != auxiliary_element_bytes(field.type)) return {};
    switch (field.type) {
        case 'c': return read_wire_signed<std::int8_t>(bytes);
        case 'C': return read_wire_unsigned<std::uint8_t>(bytes);
        case 's': return read_wire_signed<std::int16_t>(bytes);
        case 'S': return read_wire_unsigned<std::uint16_t>(bytes);
        case 'i': return read_wire_signed<std::int32_t>(bytes);
        case 'I': return read_wire_unsigned<std::uint32_t>(bytes);
        default: return {};
    }
}

/** Check only the native structure of predefined NM/MD/AS summaries.
 * Original payloads may retain malformed summaries; native projections may not.
 * MD uses the SAMtags grammar without parsing counts or requiring reference data.
 * Every other tag keeps the existing generic auxiliary validation policy.
 */
[[nodiscard]] inline bool valid_native_alignment_summary(const auxiliary_field_view& field) {
    if (field.tag == std::array{'N', 'M'}) {
        const auto differences = auxiliary_integer_value(field);
        return differences && *differences >= 0;
    }
    if (field.tag == std::array{'A', 'S'}) return auxiliary_integer_value(field).has_value();
    if (field.tag != std::array{'M', 'D'}) return true;
    if (field.type != 'Z') return false;
    const std::string_view text{reinterpret_cast<const char*>(field.payload.data()), field.payload.size()};
    std::size_t position{};
    while (position < text.size()) {
        const auto count_begin = position;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') ++position;
        if (position == count_begin) return false;
        if (position == text.size()) return true;
        if (text[position] == '^') {
            const auto deletion_begin = ++position;
            while (position < text.size() && text[position] >= 'A' && text[position] <= 'Z') ++position;
            if (position == deletion_begin) return false;
        } else {
            if (text[position] < 'A' || text[position] > 'Z') return false;
            ++position;
        }
    }
    return false;
}

/** Identify the only auxiliary summaries derived from the emitted query/reference. */
[[nodiscard]] inline bool derived_alignment_auxiliary_tag(const std::array<char, 2> tag) {
    return tag == std::array{'N', 'M'} || tag == std::array{'M', 'D'};
}

/** Retain only source tags whose meaning survives synthetic protein projection.
 * Identity/barcode strings and inherited alignment scores have no coordinates in
 * the projected read. Other source tags remain in the lossless original payload;
 * separately derived nucleotide tags are appended through their own checked path.
 */
[[nodiscard]] inline bool protein_auxiliary_is_projection_invariant(
    const auxiliary_field_view& field) {
    if (field.tag == std::array{'A', 'S'}) return auxiliary_integer_value(field).has_value();
    constexpr std::array string_tags{
        std::array{'R', 'G'}, std::array{'P', 'G'}, std::array{'B', 'C'},
        std::array{'Q', 'T'}, std::array{'R', 'X'}, std::array{'Q', 'X'},
        std::array{'M', 'I'}};
    return field.type == 'Z'
        && std::find(string_tags.begin(), string_tags.end(), field.tag) != string_tags.end();
}

/** Decide which original fields may appear unchanged in the native projection.
 * Transport/envelope tags are regenerated, malformed summaries remain payload-only,
 * SAM cannot spell nonfinite floats, and protein projection keeps invariant metadata.
 * A suppressed cross-domain mate retains MC/R2/Q2 only in the original payload:
 * those tags carry its CIGAR, sequence and sequence-indexed quality. Numeric mate
 * confidence and unrelated nucleotide summaries keep their existing policy.
 */
[[nodiscard]] inline bool retain_original_native_auxiliary(
    const auxiliary_field_view& field, const bool protein_triplets, const bool sam_text,
    const bool mate_projection_suppressed) {
    return field.tag != std::array{'b', 'q'} && field.tag != std::array{'C', 'G'}
        && (!mate_projection_suppressed || (field.tag != std::array{'M', 'C'}
            && field.tag != std::array{'R', '2'} && field.tag != std::array{'Q', '2'}))
        && valid_native_alignment_summary(field)
        && (!sam_text || !auxiliary_has_nonfinite_float(field))
        && (!protein_triplets || protein_auxiliary_is_projection_invariant(field));
}

/** Compare typed auxiliary values while ignoring legal scalar integer-width choices. */
[[nodiscard]] inline bool auxiliary_logically_equal(
    const auxiliary_field_view& expected, const auxiliary_field_view& actual) {
    if (expected.tag != actual.tag) return false;
    const auto expected_integer = auxiliary_integer_value(expected);
    const auto actual_integer = auxiliary_integer_value(actual);
    if (expected_integer || actual_integer) return expected_integer == actual_integer;
    if (expected.type != actual.type || expected.array_element_type != actual.array_element_type
        || expected.payload.size() != actual.payload.size()) return false;
    return bioseq::simd::first_unequal_sequence_rank(
        reinterpret_cast<const std::uint8_t*>(expected.payload.data()),
        reinterpret_cast<const std::uint8_t*>(actual.payload.data()),
        expected.payload.size()) == expected.payload.size();
}

/** Compare a canonical projected-aux snapshot with native BAM auxiliary bytes.
 * bq is the payload itself and valid CG is transport; all other values must match.
 * parse_bam_record has already rejected duplicates and validated CG geometry.
 */
[[nodiscard]] inline std::expected<void, std::string> validate_bam_auxiliary_projection(
    std::span<const auxiliary_field_view> expected, const bam_record_view& native) {
    std::bitset<52 * 62> expected_tags;
    for (const auto& field : expected) {
        const auto valid = validate_auxiliary_field(field);
        const auto index = auxiliary_tag_index(field.tag);
        if (!valid || !index || expected_tags.test(*index)
            || field.tag == std::array<char, 2>{'b', 'q'}
            || field.tag == std::array<char, 2>{'C', 'G'})
            return std::unexpected(std::string{"Projected auxiliary snapshot is invalid"});
        expected_tags.set(*index);
    }
    std::size_t matched{};
    for (std::size_t position = 0; position < native.auxiliary.size();) {
        const auto parsed = parse_bam_auxiliary(native.auxiliary.subspan(position));
        if (!parsed) return std::unexpected(parsed.error());
        position += parsed->consumed_bytes;
        const auto& actual = parsed->field;
        if (actual.tag == std::array<char, 2>{'b', 'q'}) continue;
        if (native.long_cigar && actual.tag == std::array<char, 2>{'C', 'G'}) continue;
        const auto found = std::find_if(expected.begin(), expected.end(),
            [tag = actual.tag](const auxiliary_field_view& field) { return field.tag == tag; });
        if (found == expected.end() || !auxiliary_logically_equal(*found, actual))
            return std::unexpected(std::string{"Projected auxiliary snapshot differs from native tag"});
        ++matched;
    }
    if (matched != expected.size())
        return std::unexpected(std::string{"Projected auxiliary snapshot has missing native tags"});
    return {};
}

/** Validate SAM header text with SIMD ASCII runs and the shared UTF-8 decoder.
 * Only SQ/RG/PG DS, PG CL and CO permit non-ASCII. CO is free one-line text,
 * including TAB, rather than a TAG:VALUE field; it still forbids NUL/CR/LF.
 */
[[nodiscard]] inline std::expected<void, std::string> validate_sam_header_value(
    const std::string_view record, const std::array<char, 2> tag, const std::string_view value) {
    const bool comment = record == "@CO";
    if (value.empty() && !comment)
        return std::unexpected(std::string{"SAM header attribute lacks a value"});
    const bool unicode_allowed = comment
        || ((record == "@SQ" || record == "@RG" || record == "@PG")
            && tag == std::array<char, 2>{'D', 'S'})
        || (record == "@PG" && tag == std::array<char, 2>{'C', 'L'});
    std::size_t position{};
    while (position < value.size()) {
        position += io::simd::scan_sam_auxiliary_prefix(
            reinterpret_cast<const std::uint8_t*>(value.data() + position), value.size() - position, false);
        if (position == value.size()) break;
        const auto character = static_cast<unsigned char>(value[position]);
        if (comment && character < 0x80u && character != '\0' && character != '\r' && character != '\n') {
            ++position;
            continue;
        }
        if (!unicode_allowed || character < 0x80u)
            return std::unexpected(std::string{"SAM header value contains an invalid character"});
        common::json_utf8_state unicode;
        do {
            if (!common::consume_json_utf8_byte(unicode, static_cast<unsigned char>(value[position++])))
                return std::unexpected(std::string{"SAM header value contains invalid UTF-8"});
        } while (unicode.continuation_bytes != 0 && position < value.size());
        if (unicode.continuation_bytes != 0)
            return std::unexpected(std::string{"SAM header value contains truncated UTF-8"});
    }
    return {};
}

/** Visit checked SAM header attributes without copying their strings. */
template<class Consumer>
[[nodiscard]] std::expected<void, std::string> visit_sam_header_fields(
    std::string_view line, Consumer&& consumer) {
    if (line.size() < 5 || line[0] != '@' || line[3] != '\t')
        return std::unexpected(std::string{"SAM header field record lacks its prefix/TAB or attributes"});
    const auto record = line.substr(0, 3);
    if (record != "@HD" && record != "@SQ" && record != "@RG" && record != "@PG")
        return std::unexpected(std::string{"Unknown SAM header field record"});
    std::bitset<52 * 62> seen;
    std::size_t cursor = 4;
    while (cursor < line.size()) {
        const auto tail = line.substr(cursor);
        const auto length = io::simd::scan_first_byte('\t',
            reinterpret_cast<const std::uint8_t*>(tail.data()), tail.size());
        const auto field = tail.substr(0, length);
        if (field.size() < 4 || field[2] != ':')
            return std::unexpected(std::string{"SAM header attribute lacks a value"});
        const std::array<char, 2> tag{field[0], field[1]};
        const auto index = auxiliary_tag_index(tag);
        if (!index || seen.test(*index))
            return std::unexpected(std::string{"SAM header attribute is invalid or duplicated"});
        seen.set(*index);
        const auto value = field.substr(3);
        if (const auto valid = validate_sam_header_value(record, tag, value); !valid) return valid;
        if (const auto consumed = std::invoke(consumer, tag, value); !consumed) return consumed;
        cursor += length;
        if (cursor < line.size()) {
            ++cursor;
            if (cursor == line.size()) return std::unexpected(std::string{"SAM header has an empty trailing attribute"});
        }
    }
    return {};
}

/** Borrowed @SQ facts; optional attributes are replayed with visit_sam_header_fields. */
struct sam_reference_header_view {
    std::string_view name;
    std::uint64_t length{};
    bool circular{};
};

/** Parse one @SQ's mandatory geometry and topology without allocating. */
[[nodiscard]] inline std::expected<sam_reference_header_view, std::string>
parse_sam_reference_header(std::string_view line) {
    if (!line.starts_with("@SQ\t"))
        return std::unexpected(std::string{"Expected a SAM @SQ reference header"});
    sam_reference_header_view reference;
    bool length_present{};
    const auto parsed = visit_sam_header_fields(line,
        [&reference, &length_present](const std::array<char, 2> tag, std::string_view value)
            -> std::expected<void, std::string> {
            if (tag == std::array<char, 2>{'S', 'N'}) reference.name = value;
            else if (tag == std::array<char, 2>{'L', 'N'}) {
                const auto length = parse_sam_integer<std::uint64_t>(value);
                if (!length || *length == 0 || *length > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
                    return std::unexpected(std::string{"SAM @SQ LN is outside signed32 bounds"});
                reference.length = *length;
                length_present = true;
            } else if (tag == std::array<char, 2>{'T', 'P'}) {
                if (value != "linear" && value != "circular")
                    return std::unexpected(std::string{"SAM @SQ topology must be linear or circular"});
                reference.circular = value == "circular";
            }
            return {};
        });
    if (!parsed) return std::unexpected(parsed.error());
    if (!valid_reference_name(reference.name) || !length_present)
        return std::unexpected(std::string{"SAM @SQ requires valid SN and LN"});
    return reference;
}


/** Borrow a logical rank slice without copying or changing its visible alphabet. */
struct alignment_rank_slice {
    const storage::borrowed_rank_window_source& original;
    std::uint64_t begin{};
    std::size_t count{};
    /** Translate a checked slice-relative read into its admitted original owner. */
    [[nodiscard]] static const std::uint8_t* read(const void* context, std::uint64_t from,
            std::size_t length, std::span<std::uint8_t> scratch) {
        const auto& slice = *static_cast<const alignment_rank_slice*>(context);
        if (from > slice.count || length > slice.count - from) return nullptr;
        const auto ranks = slice.original.read_checked(slice.begin + from, length, scratch);
        return ranks ? ranks->data() : nullptr;
    }
    /** The callback already exposes logical ranks, so the slice itself has identity orientation. */
    [[nodiscard]] storage::borrowed_rank_window_source borrow() const {
        return {this, &read, count, original.alphabet_code, storage::sequence_transform::identity, false};
    }
};

/** Bounded exact destination for one projected native sequence window. */
struct alignment_window_sink {
    std::span<std::uint8_t> destination;
    std::size_t written{};
    /** Copy synchronous encoder fragments without extending the caller's admitted tile. */
    [[nodiscard]] std::expected<void, std::string> append(std::span<const std::uint8_t> bytes) {
        if (bytes.size() > destination.size() - written)
            return std::unexpected(std::string{"projected sequence window exceeds bounded scratch"});
        std::ranges::copy(bytes, destination.begin() + static_cast<std::ptrdiff_t>(written));
        written += bytes.size();
        return {};
    }
};

/** Alphabet-erased bounded wire access; selection occurs once, not per CIGAR base. */
struct alignment_wire_source {
    storage::borrowed_rank_window_source original;
    std::uint64_t symbol_count{};
    bioseq::simd::sequence_rank_transform_kernel transform{};
    std::expected<void, std::string> (*read_at)(const alignment_wire_source&, std::uint64_t, std::span<std::uint8_t>){};

    /** Project only codons/ranks intersecting one native-orientation interval. */
    template<sequence_alphabet Alphabet, bool Reverse>
    [[nodiscard]] static std::expected<void, std::string> read_window(const alignment_wire_source& source,
            std::uint64_t from, std::span<std::uint8_t> output) {
        constexpr auto capacity = bioseq::simd::sequence_rank_io_chunk_capacity;
        constexpr std::uint64_t scale = seqan3::aminoacid_alphabet<Alphabet> ? 3u : 1u;
        if (from > source.symbol_count || output.size() > source.symbol_count - from || output.size() > capacity)
            return std::unexpected(std::string{"projected sequence interval is out of bounds"});
        const auto first = from / scale;
        const auto prefix = static_cast<std::size_t>(from % scale);
        const auto original_count = (prefix + output.size()) / scale
            + static_cast<std::size_t>((prefix + output.size()) % scale != 0u);
        const auto begin = Reverse ? source.original.symbol_count - first - original_count : first;
        const alignment_rank_slice slice{source.original, begin, original_count};
        const auto borrowed = slice.borrow();
        // Whole native windows already have their exact final extent. Only a
        // partial protein codon needs extra projected symbols beside the result.
        std::array<std::uint8_t, capacity + 6u> projected;
        const auto projected_size = original_count * scale;
        const bool direct = prefix == 0u && projected_size == output.size();
        const auto destination = direct ? output : std::span{projected}.first(projected_size);
        alignment_window_sink sink{destination};
        std::expected<std::size_t, std::string> encoded;
        if constexpr (seqan3::aminoacid_alphabet<Alphabet>)
            encoded = emit_protein_sequence<Alphabet, Reverse, false>(sink, borrowed, source.transform);
        else encoded = emit_native_sequence<Alphabet, Reverse, false>(sink, borrowed, source.transform);
        if (!encoded) return std::unexpected(encoded.error());
        if (*encoded != original_count * scale)
            return std::unexpected(std::string{"projected sequence window is incomplete"});
        if constexpr (seqan3::nucleotide_alphabet<Alphabet>) {
            constexpr bool uses_uracil = seqan3::to_char(seqan3::assign_char_to('T', Alphabet{})) == 'U';
            if constexpr (uses_uracil) {
                constexpr auto spelling = [] {
                    std::array<std::uint32_t, 256> table{};
                    for (std::size_t index{}; index < table.size(); ++index) table[index] = static_cast<std::uint32_t>(index);
                    table['U'] = 'T';
                    return table;
                }();
                if (!source.transform(bioseq::simd::sequence_rank_transform::map_bytes,
                        destination.subspan(prefix, output.size()), spelling.size(), spelling, output))
                    return std::unexpected(std::string{"RNA alignment wire spelling conversion failed"});
                return {};
            }
        }
        if (!direct) std::ranges::copy(destination.subspan(prefix, output.size()), output.begin());
        return {};
    }

    /** Bind the actual biological alphabet and compile-time strand adapter independently. */
    [[nodiscard]] static std::expected<alignment_wire_source, std::string> make(
            const storage::borrowed_rank_window_source& source, bool reverse,
            bioseq::simd::sequence_rank_transform_kernel transform) {
        if (source.alphabet_code >= supported_biological_alphabets::alphabet_count || transform == nullptr)
            return std::unexpected(std::string{"alignment derivation requires a biological source and SIMD codec"});
        alignment_wire_source result{source, source.symbol_count, transform};
        supported_biological_alphabets::dispatch_alphabet_by_code(source.alphabet_code,
            [&result, reverse]<class Alphabet>() {
                if constexpr (seqan3::aminoacid_alphabet<Alphabet>) {
                    if (result.symbol_count > std::numeric_limits<std::uint64_t>::max() / 3u) return;
                    result.symbol_count *= 3u;
                }
                result.read_at = reverse ? &read_window<Alphabet, true> : &read_window<Alphabet, false>;
            });
        if (result.read_at == nullptr) return std::unexpected(std::string{"projected sequence length overflows"});
        return result;
    }
};

/** Stream exact standard MD and return NM from real projected sequence/reference bases.
 * Only equal canonical A/C/G/T bases match; ambiguity counts as a difference.
 * Explicit =/X runs must agree wherever both bases are canonical; ambiguous
 * symbols retain the existing NM policy without imposing another match rule.
 * Insertions/deletions contribute to NM, while reference skips are absent from MD.
 */
template<alignment_byte_sink Sink>
[[nodiscard]] std::expected<std::uint64_t, std::string> derive_alignment_md(
        Sink& sink, const alignment_wire_source& query, const alignment_wire_source& reference,
        const cigar_wire_recipe& cigar, std::uint64_t reference_start, bool circular_reference) {
    constexpr auto capacity = bioseq::simd::sequence_rank_io_chunk_capacity;
    constexpr auto query_table = [] {
        std::array<std::uint32_t, 256> table{};
        for (const auto letter : std::string_view{"ACGT"}) table[static_cast<unsigned char>(letter)] = static_cast<unsigned char>(letter);
        table['U'] = 'T';
        return table;
    }();
    constexpr auto reference_table = [] {
        std::array<std::uint32_t, 256> table{};
        table.fill(255u);
        for (const auto letter : std::string_view{"ACGT"}) table[static_cast<unsigned char>(letter)] = static_cast<unsigned char>(letter);
        table['U'] = 'T';
        return table;
    }();
    if (query.read_at == nullptr || reference.read_at == nullptr || reference.symbol_count == 0u
        || reference_start >= reference.symbol_count || query.transform == nullptr)
        return std::unexpected(std::string{"alignment derivation source or reference start is invalid"});
    std::array<std::uint8_t, capacity> query_bytes{}, reference_bytes{}, comparison_bytes{};
    std::uint64_t query_position{}, reference_position = reference_start, matched{}, differences{};
    const auto advance = [](std::uint64_t& value, std::uint64_t amount) -> std::expected<void, std::string> {
        if (amount > std::numeric_limits<std::uint64_t>::max() - value)
            return std::unexpected(std::string{"alignment derivation position/count overflows"});
        value += amount;
        return {};
    };
    const auto read_reference = [&reference, circular_reference](std::uint64_t position,
            std::span<std::uint8_t> output) -> std::expected<void, std::string> {
        if (!circular_reference) return reference.read_at(reference, position, output);
        while (!output.empty()) {
            const auto begin = position % reference.symbol_count;
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(output.size(), reference.symbol_count - begin));
            if (const auto read = reference.read_at(reference, begin, output.first(count)); !read) return read;
            position = begin + count;
            output = output.subspan(count);
        }
        return {};
    };
    const auto visited = visit_wire_cigar(cigar,
        [&sink, &query, &read_reference, &query_position, &reference_position, &matched, &differences,
         &query_bytes, &reference_bytes, &comparison_bytes, &advance, &query_table, &reference_table, capacity](std::uint32_t word) -> std::expected<void, std::string> {
            const auto operation = word & 15u;
            const auto length = static_cast<std::uint64_t>(word >> 4u);
            if (operation == 0u || operation == 7u || operation == 8u) {
                for (std::uint64_t offset{}; offset < length;) {
                    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(capacity, length - offset));
                    auto query_tile = std::span{query_bytes}.first(count);
                    auto reference_tile = std::span{reference_bytes}.first(count);
                    auto comparison = std::span{comparison_bytes}.first(count);
                    if (const auto read = query.read_at(query, query_position, query_tile); !read) return read;
                    if (const auto read = read_reference(reference_position, reference_tile); !read) return read;
                    if (!query.transform(bioseq::simd::sequence_rank_transform::map_bytes, query_tile, 256u, query_table, query_tile)
                        || !query.transform(bioseq::simd::sequence_rank_transform::map_bytes, reference_tile, 256u, reference_table, comparison))
                        return std::unexpected(std::string{"alignment comparison SIMD mapping failed"});
                    std::size_t position{};
                    while (position < count) {
                        const auto equal = bioseq::simd::first_unequal_sequence_rank(query_tile.data() + position,
                            comparison.data() + position, count - position);
                        if (operation == 8u && equal != 0u)
                            return std::unexpected(std::string{"CIGAR X contradicts matching canonical sequence/reference bases"});
                        if (const auto added = advance(matched, equal); !added) return added;
                        position += equal;
                        if (position == count) break;
                        // The existing comparison tables map ambiguity to disjoint sentinels.
                        if (operation == 7u && query_tile[position] != 0u && comparison[position] != 255u)
                            return std::unexpected(std::string{"CIGAR = contradicts mismatching canonical sequence/reference bases"});
                        if (auto written = append_sam_integer(sink, matched); !written) return written;
                        if (auto written = sink.append(std::span<const std::uint8_t>{reference_tile}.subspan(position, 1u)); !written) return written;
                        matched = 0u;
                        if (const auto added = advance(differences, 1u); !added) return added;
                        ++position;
                    }
                    if (const auto moved = advance(query_position, count); !moved) return moved;
                    if (const auto moved = advance(reference_position, count); !moved) return moved;
                    offset += count;
                }
            } else if (operation == 2u) {
                if (auto written = append_sam_integer(sink, matched); !written) return written;
                constexpr std::array<std::uint8_t, 1> deletion{'^'};
                if (auto written = sink.append(deletion); !written) return written;
                matched = 0u;
                for (std::uint64_t offset{}; offset < length;) {
                    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(capacity, length - offset));
                    auto reference_tile = std::span{reference_bytes}.first(count);
                    if (const auto read = read_reference(reference_position, reference_tile); !read) return read;
                    if (auto written = sink.append(std::span<const std::uint8_t>{reference_tile}); !written) return written;
                    if (const auto moved = advance(reference_position, count); !moved) return moved;
                    offset += count;
                }
                if (const auto added = advance(differences, length); !added) return added;
            } else if (operation == 1u || operation == 4u) {
                if (const auto moved = advance(query_position, length); !moved) return moved;
                if (operation == 1u)
                    if (const auto added = advance(differences, length); !added) return added;
            } else if (operation == 3u) {
                if (const auto moved = advance(reference_position, length); !moved) return moved;
            }
            return {};
        });
    if (!visited) return std::unexpected(visited.error());
    if (query_position != query.symbol_count)
        return std::unexpected(std::string{"MD derivation CIGAR does not consume the complete query"});
    if (auto written = append_sam_integer(sink, matched); !written) return std::unexpected(written.error());
    return differences;
}

} // namespace bioseq::io::alignment
