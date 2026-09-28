/**
 * @file index.cppm
 * @brief Reads FASTA and FASTQ header fields into destination columns.
 *
 * The parser finds delimiters, extracts fields for the selected header format,
 * and sends each value to the columns chosen when the load starts. Writers
 * receive calls through function pointers, so parsing code is shared across
 * writer types.
 */
module;

#include <algorithm>
#include <array>
#include <boost/container/small_vector.hpp>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.io.identifier_parser;

import bioseq.common.logging;
import bioseq.io.simd;      // scan_for_each_delim4 (one SIMD pass per header)
import bioseq.io.file_kind; // bioseq::io::detail::lowercase_ascii_character (shared ASCII fold)
import bioseq.io.identifier_parser.formats;

static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::io::identifier_parser {

// Column mappings and field routes.

/// Zero-based index of a writer  destination.
using destination_slot = std::int32_t;

/// One entry of the SQL `columns => ARRAY[(column_name, data_source)]` argument.
/// The caller flattens the composite into borrowed `string_view`s that remain
/// valid until `resolve_identifier_mapping` returns.
struct user_column_mapping {
    std::string_view column_name;
    std::string_view data_source;
};

/// One non-sequence destination column described without database types.
struct column_descriptor {
    std::string_view name; // raw column name
    bool is_text;          // TEXT, VARCHAR, or BPCHAR column eligible to, hold the header
    bool is_primary_key;
    bool not_null;
    bool has_default;
    bool is_identity_or_generated;
};

/// Sends a named header field to one or more destination columns.
/// The loader builds these routes once and reuses them for each record.
struct identifier_key_route {
    std::string key;
    std::vector<destination_slot> slots;
};

/// Routes one positional field index to one or more destination slots.
struct identifier_position_route {
    std::uint32_t position;
    std::vector<destination_slot> slots;
};

/// Complete routing table resolved once for a sequence load.
struct identifier_routing {
    std::vector<identifier_key_route> key_routes;
    std::vector<identifier_position_route> position_routes;
    std::vector<destination_slot> identifier_slots;
    std::size_t destination_count = 0; // number of distinct slots
    bool strip_metadata = false;       // remove recognized tags and bracketed text from the identifier
};

/// What `resolve_identifier_mapping` returns. `slot_to_column` maps each slot to the
/// caller's column index (an index into the `columns` span passed in).
struct identifier_mapping_result {
    identifier_routing routing;
    std::vector<std::uint16_t> slot_to_column; // slot -> index into input columns
};

// Writer interface and function-pointer adapter.

/// Writer operations used while loading parsed header fields.
/// `set_field` receives borrowed bytes and converts them to the destination type.
/// The loader starts and ends each tuple around the call to the header parser.
template <typename p_writer>
concept identifier_destination_writer = requires(
    p_writer& w, const p_writer& cw, destination_slot slot, const char* data, std::int32_t len) {
    { w.begin_tuple() } -> std::same_as<void>;
    { w.set_field(slot, data, len) } -> std::same_as<void>; // writer converts the field bytes to the destination type
    { w.set_null(slot) } -> std::same_as<void>;
    { w.end_tuple() } -> std::same_as<void>;
    { w.finish() } -> std::same_as<void>;
    { cw.destination_count() } -> std::convertible_to<std::size_t>;
};

/// Reference/no-op writer used by the compile-time concept check.
struct example_identifier_destination_writer {
    std::size_t n = 0;
    void begin_tuple() noexcept {}
    void set_field(destination_slot, const char*, std::int32_t) noexcept {}
    void set_null(destination_slot) noexcept {}
    void end_tuple() noexcept {}
    void finish() noexcept {}
    [[nodiscard]] std::size_t destination_count() const noexcept {
        return n;
    }
};
static_assert(identifier_destination_writer<example_identifier_destination_writer>);

/// Borrows a writer through function pointers.
/// This lets every  writer ype share the same parsing code. The writer must
/// remain alive while this reference is used.
struct identifier_writer_reference {
    void* self = nullptr;
    void (*begin_tuple_callback)(void*) = nullptr;
    void (*set_field_callback)(void*, destination_slot, const char*, std::int32_t) = nullptr;
    void (*set_null_callback)(void*, destination_slot) = nullptr;
    void (*end_tuple_callback)(void*) = nullptr;
    void (*finish_callback)(void*) = nullptr;
    std::size_t (*destination_count_callback)(const void*) = nullptr;

    void begin_tuple() const {
        begin_tuple_callback(self);
    }
    void set_field(destination_slot slot, const char* data, std::int32_t length) const {
        set_field_callback(self, slot, data, length);
    }
    void set_null(destination_slot slot) const {
        set_null_callback(self, slot);
    }
    void end_tuple() const {
        end_tuple_callback(self);
    }
    void finish() const {
        finish_callback(self);
    }
    [[nodiscard]] std::size_t destination_count() const {
        return destination_count_callback(self);
    }
};

/// Builds a function-pointer adapter that borrows `w`.
template <identifier_destination_writer p_writer>
[[nodiscard]] identifier_writer_reference make_identifier_writer_reference(p_writer& w) noexcept {
    return identifier_writer_reference{
        .self = &w,
        .begin_tuple_callback = [](void* writer) { static_cast<p_writer*>(writer)->begin_tuple(); },
        .set_field_callback =
            [](void* writer, destination_slot slot, const char* data, std::int32_t length) {
                static_cast<p_writer*>(writer)->set_field(slot, data, length);
            },
        .set_null_callback =
            [](void* writer, destination_slot slot) {
                static_cast<p_writer*>(writer)->set_null(slot);
            },
        .end_tuple_callback = [](void* writer) { static_cast<p_writer*>(writer)->end_tuple(); },
        .finish_callback = [](void* writer) { static_cast<p_writer*>(writer)->finish(); },
        .destination_count_callback =
            [](const void* writer) {
                return static_cast<const p_writer*>(writer)->destination_count();
            },
    };
}

// Header tokens and reusable parser state.

/// Structural delimiters recognized during one header scan.
enum class delimiter_kind : std::uint8_t { pipe, equals, left_bracket, right_bracket };
/// Offset and kind of one structural header delimiter.
struct delimiter_token {
    std::uint32_t offset;
    delimiter_kind kind;
};

/// Collects header delimiters in byte order with one SIMD scan.
/// Stores up to 48 tokens without a separate allocation
struct header_lexer {
    boost::container::small_vector<delimiter_token, 48> tokens;

    /// Replace the token list with delimiters found in `header`.
    void lex(std::string_view header) {
        tokens.clear();
        /// Borrowed callback state for the SIMD delimiter scan.
        struct lexer_context {
            header_lexer* self;
            const char* base;
        };
        lexer_context ctx{this, header.data()};
        bioseq::io::simd::scan_for_each_delim4(
            '|',
            '=',
            '[',
            ']',
            reinterpret_cast<const std::uint8_t*>(header.data()),
            header.size(),
            &ctx,
            [](void* p, std::size_t offset) {
                auto* c = static_cast<lexer_context*>(p);
                const char ch = c->base[offset];
                const delimiter_kind k = ch == '|'   ? delimiter_kind::pipe
                                         : ch == '=' ? delimiter_kind::equals
                                         : ch == '[' ? delimiter_kind::left_bracket
                                                     : delimiter_kind::right_bracket;
                c->self->tokens.push_back({static_cast<std::uint32_t>(offset), k});
            });
    }
};

/// Scratch storage the caller can reuse between headers.
/// Token and string buffers may grow when a larger header arrives.
struct identifier_parse_state {
    header_lexer lexer;
    std::string strip_scratch;
};

} // namespace bioseq::io::identifier_parser

// Parsing helpers.

namespace bioseq::io::identifier_parser::detail {

using std::string_view;
namespace bc = boost::container;

/// Return whether a byte is valid inside an equals-tag key.
[[nodiscard]] constexpr bool is_key_char(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

/// Compares names while ignoring ASCII case, underscores, and hyphens.
/// Reads both strings directly without a temporary  buffer.
[[nodiscard]] constexpr bool names_match_normalised(string_view a, string_view b) noexcept {
    std::size_t i = 0, j = 0;
    // Consume the next normalized name byte while skipping separators.
    auto advance = [](string_view s, std::size_t& k) -> int {
        while (k < s.size()) {
            const char c = s[k++];
            if (c == '_' || c == '-')
                continue;
            return static_cast<unsigned char>(bioseq::io::detail::lowercase_ascii_character(c));
        }
        return -1;
    };
    for (;;) {
        const int ca = advance(a, i);
        const int cb = advance(b, j);
        if (ca != cb)
            return false;
        if (ca == -1)
            return true;
    }
}

/// Returns an owned name with ASCII letters lowercased and '_' and '-' removed.
[[nodiscard]] inline std::string normalise_to_string(string_view in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '_' || c == '-')
            continue;
        out.push_back(bioseq::io::detail::lowercase_ascii_character(c));
    }
    return out;
}

/// Trim leading ASCII spaces from a borrowed view.
[[nodiscard]] constexpr string_view ltrim_spaces(string_view s) noexcept {
    std::size_t b = 0;
    while (b < s.size() && s[b] == ' ')
        ++b;
    return s.substr(b);
}
/// Trim trailing ASCII spaces from a borrowed view.
[[nodiscard]] constexpr string_view rtrim_spaces(string_view s) noexcept {
    std::size_t e = s.size();
    while (e > 0 && s[e - 1] == ' ')
        --e;
    return s.substr(0, e);
}
/// Trim leading and trailing ASCII spaces from a borrowed view.
[[nodiscard]] constexpr string_view trim_spaces(string_view s) noexcept {
    return rtrim_spaces(ltrim_spaces(s));
}

/// Splits a header into borrowed fields, preserving empty fields.
/// Pipe positions come from the lexer. Other delimiters are scanned here.
[[nodiscard]] inline bc::small_vector<string_view, 16>
positional_split_on(char delim, string_view header, const header_lexer& lex) {
    bc::small_vector<string_view, 16> out;
    std::size_t start = 0;
    if (delim == '|') {
        for (const auto& t : lex.tokens) {
            if (t.kind == delimiter_kind::pipe) {
                out.push_back(header.substr(start, t.offset - start));
                start = t.offset + 1;
            }
        }
    } else {
        for (std::size_t i = 0; i < header.size(); ++i) {
            if (header[i] == delim) {
                out.push_back(header.substr(start, i - start));
                start = i + 1;
            }
        }
    }
    out.push_back(header.substr(start));
    return out;
}

/// Split at the first space. The tail is empty when no space exists.
[[nodiscard]] inline std::pair<string_view, string_view> split_first_space(string_view s) {
    const std::size_t pos = bioseq::io::simd::scan_first_byte(
        ' ', reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    if (pos >= s.size())
        return {s, string_view{}};
    return {s.substr(0, pos), s.substr(pos + 1)};
}

/// Pairs an opening '[' with the next ']'. A later '[' replaces the open one.
/// Returns their offsets and ignores unmatched brackets.
[[nodiscard]] inline bc::small_vector<std::pair<std::uint32_t, std::uint32_t>, 8>
bracket_spans(const header_lexer& lex) {
    bc::small_vector<std::pair<std::uint32_t, std::uint32_t>, 8> out;
    std::optional<std::uint32_t> open;
    for (const auto& t : lex.tokens) {
        if (t.kind == delimiter_kind::left_bracket)
            open = t.offset;
        else if (t.kind == delimiter_kind::right_bracket && open) {
            out.push_back({*open, t.offset});
            open.reset();
        }
    }
    return out;
}

/// Return whether an offset lies strictly inside any bracket span.
[[nodiscard]] inline bool
offset_in_any_bracket(const bc::small_vector<std::pair<std::uint32_t, std::uint32_t>, 8>& spans,
                      std::uint32_t e) {
    for (const auto& [lb, rb] : spans)
        if (lb < e && e < rb)
            return true;
    return false;
}

/// Accepted equals-tag delimiter and its optional fixed-format field.
struct accepted_eq {
    std::uint32_t ks;                     // key-run start
    std::uint32_t eq;                     // '=' offset
    const header_field_definition* field; // matched equals_tag field (fixed) or nullptr (AUTO)
};

/// Finds KEY=VALUE tags outside brackets whose '=' is in [begin,end).
/// Keys contain letters or digits and start at the header's beginning or after
/// a space or pipe. Fixed formats accept only their declared equals-tag fields.
/// Returns tags in, header order.
[[nodiscard]] inline bc::small_vector<accepted_eq, 16>
collect_accepted_equals(string_view header,
                        const header_lexer& lex,
                        std::uint32_t begin,
                        std::uint32_t end,
                        const header_format_definition& def,
                        bool accept_any) {
    const auto spans = bracket_spans(lex);
    bc::small_vector<accepted_eq, 16> out;
    for (const auto& t : lex.tokens) {
        if (t.kind != delimiter_kind::equals)
            continue;
        const std::uint32_t e = t.offset;
        if (e < begin || e >= end)
            continue;
        if (offset_in_any_bracket(spans, e))
            continue;
        if (e == 0)
            continue;
        std::uint32_t ks = e;
        while (ks > 0 && is_key_char(header[ks - 1]))
            --ks;
        if (ks == e)
            continue; // empty key run
        const bool boundary = (ks == 0) || header[ks - 1] == ' ' || header[ks - 1] == '|';
        if (!boundary)
            continue;
        const header_field_definition* field = nullptr;
        if (!accept_any) {
            const string_view key = header.substr(ks, e - ks);
            for (const auto& fd : def.fields) {
                if (fd.origin != field_origin::equals_tag)
                    continue;
                if (names_match_normalised(key, fd.canonical) ||
                    (!fd.token.empty() && names_match_normalised(key, fd.token))) {
                    field = &fd;
                    break;
                }
            }
            if (field == nullptr)
                continue; // unknown field for this format
        }
        out.push_back({ks, e, field});
    }
    return out;
}

/// Borrowed key, value, and optional fixed-format field for one equals tag.
struct equals_tag {
    string_view key;
    string_view value; // right-trimmed
    const header_field_definition* field;
};

/// Return accepted equals tags with right-trimmed values. Each
/// value runs from just after its '=' to the next accepted tag's key start (or
/// `end`), then trailing spaces are removed.
[[nodiscard]] inline bc::small_vector<equals_tag, 16>
equals_tags(string_view header,
            const header_lexer& lex,
            std::uint32_t begin,
            std::uint32_t end,
            const header_format_definition& def,
            bool accept_any) {
    const auto acc = collect_accepted_equals(header, lex, begin, end, def, accept_any);
    bc::small_vector<equals_tag, 16> out;
    for (std::size_t i = 0; i < acc.size(); ++i) {
        const std::uint32_t vstart = acc[i].eq + 1;
        const std::uint32_t vend = (i + 1 < acc.size()) ? acc[i + 1].ks : end;
        const string_view value =
            rtrim_spaces(header.substr(vstart, vend > vstart ? (vend - vstart) : 0));
        const string_view key = header.substr(acc[i].ks, acc[i].eq - acc[i].ks);
        out.push_back({key, value, acc[i].field});
    }
    return out;
}

/// Finds where the description ends at an accepted tag or paired bracket.
/// Searches [begin,end) and returns `end` when neither  is present.
[[nodiscard]] inline std::uint32_t first_tag_offset(string_view header,
                                                    const header_lexer& lex,
                                                    std::uint32_t begin,
                                                    std::uint32_t end,
                                                    const header_format_definition& def,
                                                    bool accept_any) {
    std::uint32_t first = end;
    const auto acc = collect_accepted_equals(header, lex, begin, end, def, accept_any);
    if (!acc.empty())
        first = std::min(first, acc[0].ks);
    for (const auto& [lb, rb] : bracket_spans(lex)) {
        (void)rb;
        if (lb >= begin && lb < end)
            first = std::min(first, lb);
    }
    return first;
}

/// Reads key/value pairs from bracketed text containing exactly one equals sign.
/// Removes surrounding spaces from both parts.
[[nodiscard]] inline bc::small_vector<std::pair<string_view, string_view>, 8>
bracket_tags(string_view header, const header_lexer& lex) {
    bc::small_vector<std::pair<string_view, string_view>, 8> out;
    for (const auto& [lb, rb] : bracket_spans(lex)) {
        int eq_count = 0;
        std::uint32_t epos = 0;
        for (const auto& t : lex.tokens)
            if (t.kind == delimiter_kind::equals && t.offset > lb && t.offset < rb) {
                ++eq_count;
                epos = t.offset;
            }
        if (eq_count == 1) {
            const string_view key = trim_spaces(header.substr(lb + 1, epos - (lb + 1)));
            const string_view val = trim_spaces(header.substr(epos + 1, rb - (epos + 1)));
            out.push_back({key, val});
        }
    }
    return out;
}

/// Return trimmed contents of the last bracket span that
/// contains no '=' (the ncbi/RefSeq value-only `[organism]` convention).
[[nodiscard]] inline string_view trailing_bracket_value(string_view header,
                                                        const header_lexer& lex) {
    string_view result;
    for (const auto& [lb, rb] : bracket_spans(lex)) {
        bool has_eq = false;
        for (const auto& t : lex.tokens)
            if (t.kind == delimiter_kind::equals && t.offset > lb && t.offset < rb) {
                has_eq = true;
                break;
            }
        if (!has_eq)
            result = trim_spaces(header.substr(lb + 1, rb - (lb + 1)));
    }
    return result;
}

/// Return the start of the last value-only bracket span, or `npos`.
[[nodiscard]] inline std::size_t trailing_bracket_start(string_view header,
                                                        const header_lexer& lex) {
    std::size_t result = string_view::npos;
    for (const auto& [lb, rb] : bracket_spans(lex)) {
        (void)header;
        bool has_eq = false;
        for (const auto& t : lex.tokens)
            if (t.kind == delimiter_kind::equals && t.offset > lb && t.offset < rb) {
                has_eq = true;
                break;
            }
        if (!has_eq)
            result = lb;
    }
    return result;
}

/// Remove every accepted equals tag and bracket span, collapse repeated spaces,
/// trim both ends, and write the result into caller-owned scratch storage.
[[nodiscard]] inline string_view strip_tags(string_view header,
                                            const header_lexer& lex,
                                            std::string& scratch,
                                            const header_format_definition& def,
                                            bool accept_any) {
    bc::small_vector<std::pair<std::uint32_t, std::uint32_t>, 16> remove; // [start,end)
    const auto acc = collect_accepted_equals(
        header, lex, 0, static_cast<std::uint32_t>(header.size()), def, accept_any);
    for (std::size_t i = 0; i < acc.size(); ++i) {
        const std::uint32_t start = acc[i].ks;
        const std::uint32_t end =
            (i + 1 < acc.size()) ? acc[i + 1].ks : static_cast<std::uint32_t>(header.size());
        remove.push_back({start, end});
    }
    for (const auto& [lb, rb] : bracket_spans(lex))
        remove.push_back({lb, rb + 1});
    std::sort(remove.begin(), remove.end());

    scratch.clear();
    // Append remaining description text while collapsing repeated spaces.
    auto append_collapsed = [&scratch](string_view chunk) {
        for (char c : chunk) {
            if (c == ' ') {
                if (!scratch.empty() && scratch.back() != ' ')
                    scratch.push_back(' ');
            } else {
                scratch.push_back(c);
            }
        }
    };
    std::size_t cursor = 0;
    for (const auto& [start, end] : remove) {
        if (start > cursor)
            append_collapsed(header.substr(cursor, start - cursor));
        if (end > cursor)
            cursor = end;
    }
    if (cursor < header.size())
        append_collapsed(header.substr(cursor));
    while (!scratch.empty() && scratch.back() == ' ')
        scratch.pop_back();
    return string_view{scratch};
}

// Sending parsed values to the writer.

/// Find destination slots for a normalized field key.
[[nodiscard]] inline const std::vector<destination_slot>*
find_key_slots(const identifier_routing& routing, string_view key) noexcept {
    for (const auto& route : routing.key_routes) {
        if (string_view{route.key} == key)
            return &route.slots;
    }
    return nullptr;
}

/// Find destination slots for a positional field index.
[[nodiscard]] inline const std::vector<destination_slot>*
find_position_slots(const identifier_routing& routing, std::uint32_t position) noexcept {
    for (const auto& route : routing.position_routes) {
        if (route.position == position)
            return &route.slots;
    }
    return nullptr;
}

/// Sends extrcted header values to the columns selected by the routing table.
struct route_emitter {
    const identifier_routing& routing;
    identifier_writer_reference writer;
    std::array<char, 256> key_buffer{}; // buffer for normalizing raw header keys

    /// Write a nonempty value or set the destination slot to null.
    void write_or_null(destination_slot slot, string_view value) const {
        if (value.empty())
            writer.set_null(slot);
        else
            writer.set_field(slot, value.data(), static_cast<std::int32_t>(value.size()));
    }
    /// Sends a field using its already normalized key
    void keyed(string_view normalised_key, string_view value) const {
        if (const auto* slots = find_key_slots(routing, normalised_key)) {
            for (destination_slot s : *slots)
                write_or_null(s, value);
        }
    }
    /// Normalizes a raw header key before looking up its destinations.
    void keyed_raw(string_view raw_key, string_view value) {
        if (raw_key.size() <= key_buffer.size()) {
            keyed(normalise_name(raw_key, key_buffer), value);
        } else {
            const std::string nk = normalise_to_string(raw_key);
            keyed(nk, value);
        }
    }
    /// Route one positional field value.
    void positional(std::uint32_t idx, string_view value) const {
        if (const auto* slots = find_position_slots(routing, idx)) {
            for (destination_slot s : *slots)
                write_or_null(s, value);
        }
    }
    /// Route the selected identifier value to every identifier slot.
    void identifier(string_view value) const {
        for (destination_slot s : routing.identifier_slots)
            write_or_null(s, value);
    }
};

/// Sends the whole header to identifier columns, optionally removing metadata.
inline void emit_identifier(string_view header,
                            identifier_parse_state& st,
                            const header_format_definition& def,
                            bool accept_any,
                            const identifier_routing& routing,
                            route_emitter& emit) {
    if (routing.identifier_slots.empty())
        return;
    if (routing.strip_metadata)
        emit.identifier(strip_tags(header, st.lexer, st.strip_scratch, def, accept_any));
    else
        emit.identifier(header);
}

/// Find a format field by canonical name, token, or alias.
[[nodiscard]] inline const header_field_definition*
find_field_for_name(const header_format_definition& def, string_view name) {
    for (const auto& fd : def.fields) {
        if (names_match_normalised(name, fd.canonical))
            return &fd;
        if (!fd.token.empty() && names_match_normalised(name, fd.token))
            return &fd;
        for (const string_view alias : fd.aliases)
            if (names_match_normalised(name, alias))
                return &fd;
    }
    return nullptr;
}

// Parsers for individual header formats.

/// Reads a UniProtKB header such as `sp|accession|entry description OS=organism`.
/// Sends the  prefix, description, and recognized tags to their column routes.
inline void parse_uniprot(string_view header,
                          identifier_parse_state& st,
                          const header_format_definition& def,
                          route_emitter& emit) {
    const header_lexer& lex = st.lexer;
    const auto parts = positional_split_on('|', header, lex);
    if (parts.size() >= 1) {
        const string_view db = trim_spaces(parts[0]);
        emit.positional(0, db);
        emit.keyed("db", db);
    }
    if (parts.size() >= 2) {
        const string_view acc = trim_spaces(parts[1]);
        emit.positional(1, acc);
        emit.keyed("accession", acc);
    }
    string_view after{header.data() + header.size(), 0}; // empty by default
    if (parts.size() >= 3) {
        const auto [entry, tail] = split_first_space(parts[2]);
        emit.positional(2, entry);
        emit.keyed("entryname", entry);
        const std::size_t us = entry.find('_');
        if (us != string_view::npos && entry.find('_', us + 1) == string_view::npos) {
            emit.keyed("geneid", entry.substr(0, us));
            emit.keyed("taxon", entry.substr(us + 1));
        }
        after = tail;
    }
    const std::uint32_t begin = static_cast<std::uint32_t>(after.data() - header.data());
    const std::uint32_t end = begin + static_cast<std::uint32_t>(after.size());
    const auto tags = equals_tags(header, lex, begin, end, def, /*accept_any=*/false);
    const std::uint32_t desc_end =
        first_tag_offset(header, lex, begin, end, def, /*accept_any=*/false);
    const string_view protein_name = rtrim_spaces(header.substr(begin, desc_end - begin));
    emit.keyed("proteinname", protein_name);
    for (const auto& tag : tags)
        if (tag.field)
            emit.keyed(tag.field->normalised_key, tag.value);
    emit_identifier(header, st, def, /*accept_any=*/false, emit.routing, emit);
}

/// Parse a first-token identifier and equals-tag metadata.
inline void parse_keyed(string_view header,
                        identifier_parse_state& st,
                        const header_format_definition& def,
                        route_emitter& emit) {
    const header_lexer& lex = st.lexer;
    const auto [id, after] = split_first_space(header);
    emit.positional(0, id);
    emit.keyed("id", id);
    const std::uint32_t begin = static_cast<std::uint32_t>(after.data() - header.data());
    const std::uint32_t end = begin + static_cast<std::uint32_t>(after.size());
    const auto tags = equals_tags(header, lex, begin, end, def, /*accept_any=*/false);

    const header_field_definition* fd_desc = nullptr;
    for (const auto& fd : def.fields)
        if (fd.origin == field_origin::description) {
            fd_desc = &fd;
            break;
        }
    if (fd_desc) {
        const std::uint32_t desc_end =
            first_tag_offset(header, lex, begin, end, def, /*accept_any=*/false);
        emit.keyed(fd_desc->normalised_key, rtrim_spaces(header.substr(begin, desc_end - begin)));
    }
    for (const auto& tag : tags)
        if (tag.field)
            emit.keyed(tag.field->normalised_key, tag.value);
    emit_identifier(header, st, def, /*accept_any=*/false, emit.routing, emit);
}

/// Reads delimiter-separated fields for SPD, GISAID, and IMGT headers.
inline void parse_positional(string_view header,
                             identifier_parse_state& st,
                             const header_format_definition& def,
                             route_emitter& emit) {
    const header_lexer& lex = st.lexer;
    const auto parts = positional_split_on(def.primary_delimiter, header, lex);
    for (std::size_t i = 0; i < parts.size(); ++i)
        emit.positional(static_cast<std::uint32_t>(i), trim_spaces(parts[i]));
    for (const auto& fd : def.fields)
        if (fd.origin == field_origin::positional && fd.position < parts.size())
            emit.keyed(fd.normalised_key, trim_spaces(parts[fd.position]));
    // SPD stores gene_id and taxon together in the gene field, separated by '_'.
    if (def.id == header_format::spd && parts.size() > 1) {
        const string_view gene = trim_spaces(parts[1]);
        const std::size_t us = gene.find('_');
        if (us != string_view::npos) {
            emit.keyed("geneid", gene.substr(0, us));
            emit.keyed("taxon", gene.substr(us + 1));
        }
    }
    emit_identifier(header, st, def, /*accept_any=*/false, emit.routing, emit);
}

/// Reads NCBI identifiers, descriptions, and a bracketed organism name.
/// PDB headers also provide a chain before the description.
inline void parse_ncbi(string_view header,
                       identifier_parse_state& st,
                       const header_format_definition& def,
                       route_emitter& emit) {
    const header_lexer& lex = st.lexer;
    string_view rest;
    std::uint32_t rest_begin = static_cast<std::uint32_t>(header.size());

    if (def.primary_delimiter == '|') {
        const auto parts = positional_split_on('|', header, lex);
        for (std::size_t i = 0; i < parts.size(); ++i)
            emit.positional(static_cast<std::uint32_t>(i), trim_spaces(parts[i]));
        for (const auto& fd : def.fields)
            if (fd.origin == field_origin::positional && fd.position < parts.size())
                emit.keyed(fd.normalised_key, trim_spaces(parts[fd.position]));
        const std::size_t idblk = def.identifier_block_field_count;
        if (def.id == header_format::ncbi_pdb && idblk < parts.size()) {
            const auto [chain, tail] = split_first_space(parts[idblk]);
            emit.keyed("chain", trim_spaces(chain));
            rest = tail;
            rest_begin = static_cast<std::uint32_t>(tail.data() - header.data());
        } else if (idblk < parts.size()) {
            rest = parts[idblk];
            rest_begin = static_cast<std::uint32_t>(rest.data() - header.data());
        }
    } else {
        const auto [id, tail] = split_first_space(header);
        emit.positional(0, id);
        emit.keyed("id", id);
        rest = tail;
        rest_begin = static_cast<std::uint32_t>(tail.data() - header.data());
    }
    (void)rest;

    const std::size_t bstart = trailing_bracket_start(header, lex);
    const std::uint32_t desc_end = (bstart != string_view::npos && bstart >= rest_begin)
                                       ? static_cast<std::uint32_t>(bstart)
                                       : static_cast<std::uint32_t>(header.size());
    emit.keyed("description", rtrim_spaces(header.substr(rest_begin, desc_end - rest_begin)));
    const string_view org = trailing_bracket_value(header, lex);
    if (!org.empty()) {
        emit.keyed("organism", org);
        emit.keyed("taxon", org);
    }
    emit_identifier(header, st, def, /*accept_any=*/false, emit.routing, emit);
}

/// Parse a GenBank identifier and bracketed key-value tags.
inline void parse_genbank(string_view header,
                          identifier_parse_state& st,
                          const header_format_definition& def,
                          route_emitter& emit) {
    const header_lexer& lex = st.lexer;
    const auto [id, after] = split_first_space(header);
    (void)after;
    emit.positional(0, id);
    for (const auto& [key, value] : bracket_tags(header, lex))
        emit.keyed_raw(key, value);
    emit_identifier(header, st, def, /*accept_any=*/false, emit.routing, emit);
}

/// Parse a plain header as an identifier only.
inline void parse_plain(string_view header,
                        identifier_parse_state& st,
                        const header_format_definition& def,
                        route_emitter& emit) {
    emit_identifier(header, st, def, /*accept_any=*/false, emit.routing, emit);
}

/// Parse generic positional and keyed metadata without a fixed format.
inline void parse_auto(string_view header,
                       identifier_parse_state& st,
                       const header_format_definition& def,
                       route_emitter& emit) {
    const header_lexer& lex = st.lexer;
    const auto parts = positional_split_on('|', header, lex);
    for (std::size_t i = 0; i < parts.size(); ++i)
        emit.positional(static_cast<std::uint32_t>(i), trim_spaces(parts[i]));
    const auto tags = equals_tags(
        header, lex, 0, static_cast<std::uint32_t>(header.size()), def, /*accept_any=*/true);
    for (const auto& tag : tags)
        emit.keyed_raw(tag.key, tag.value);
    for (const auto& [key, value] : bracket_tags(header, lex))
        emit.keyed_raw(key, value);
    const string_view org = trailing_bracket_value(header, lex);
    if (!org.empty()) {
        emit.keyed("organism", org);
        emit.keyed("taxon", org);
    }
    emit_identifier(header, st, def, /*accept_any=*/true, emit.routing, emit);
}

// Header format detection.

/// Count pipe delimiters in a header.
[[nodiscard]] inline std::size_t pipe_count(string_view h) {
    std::size_t count = 0;
    for (char c : h)
        if (c == '|')
            ++count;
    return count;
}

/// Recognizes three digit groups separated by slashes or hyphens.
/// Checks the shape only, so invalid calendar dates can match.
[[nodiscard]] inline bool is_date_like(string_view s) {
    if (s.empty())
        return false;
    int separators = 0;
    std::size_t group_len = 0;
    int groups = 0;
    for (char c : s) {
        if (c == '/' || c == '-') {
            if (group_len == 0)
                return false; // empty group
            ++separators;
            ++groups;
            group_len = 0;
            if (separators > 2)
                return false;
        } else if (c >= '0' && c <= '9') {
            ++group_len;
        } else {
            return false; // non-date byte
        }
    }
    if (group_len == 0)
        return false; // trailing separator
    ++groups;
    return separators == 2 && groups == 3;
}

/// Checks for an alphanumeric KEY= after a space or pipe, or at the start.
[[nodiscard]] inline bool has_equals_tag(string_view h) {
    // Recognize tags such as ' X=' and 'X=' at the start.
    for (std::size_t i = 0; i < h.size(); ++i)
        if (h[i] == '=' && i > 0 && is_key_char(h[i - 1])) {
            std::size_t k = i;
            while (k > 0 && is_key_char(h[k - 1]))
                --k;
            if (k == 0 || h[k - 1] == ' ' || h[k - 1] == '|')
                return true;
        }
    return false;
}

/// Return whether a header contains a bracketed equals tag.
[[nodiscard]] inline bool has_bracket_equals(string_view h) {
    std::size_t lb = h.find('[');
    while (lb != string_view::npos) {
        const std::size_t rb = h.find(']', lb + 1);
        if (rb == string_view::npos)
            break;
        if (h.substr(lb + 1, rb - lb - 1).find('=') != string_view::npos)
            return true;
        lb = h.find('[', rb + 1);
    }
    return false;
}

/// Return whether the last bracket span contains a value without a key.
[[nodiscard]] inline bool has_trailing_value_bracket(string_view h) {
    const std::size_t rb = h.rfind(']');
    if (rb == string_view::npos)
        return false;
    const std::size_t lb = h.rfind('[', rb);
    if (lb == string_view::npos)
        return false;
    return h.substr(lb + 1, rb - lb - 1).find('=') == string_view::npos;
}

/// Checks for an initial token beginning with two uppercase letters and an underscore.
/// Rejects tokens containing a pipe. Used as a RefSeq format hint.
[[nodiscard]] inline bool looks_like_refseq_accession(string_view h) {
    const auto [first, rest] = split_first_space(h);
    (void)rest;
    // [A-Z]{2}_ prefix, no '|' in the first token.
    if (first.size() < 3)
        return false;
    if (first.find('|') != string_view::npos)
        return false;
    return first[0] >= 'A' && first[0] <= 'Z' && first[1] >= 'A' && first[1] <= 'Z' &&
           first[2] == '_';
}

/// Checks whether a header has the features used  to recognize a format.
[[nodiscard]] inline bool format_matches(header_format f, string_view h) {
    const std::size_t pipes = pipe_count(h);
    switch (f) {
    case header_format::uniprotkb:
        return pipes >= 2 && has_equals_tag(h) &&
               (h.find("OS=") != string_view::npos || h.find("OX=") != string_view::npos ||
                h.find("GN=") != string_view::npos || h.find("PE=") != string_view::npos ||
                h.find("SV=") != string_view::npos);
    case header_format::uniref:
        return h.starts_with("UniRef") && h.find(" Tax=") != string_view::npos &&
               h.find(" n=") != string_view::npos;
    case header_format::uniparc:
        return h.starts_with("UPI") &&
               (h.find(" status=") != string_view::npos || h.find("PC=") != string_view::npos);
    case header_format::unimes:
        return h.find("OS=") != string_view::npos && h.find("SV=") != string_view::npos &&
               h.find("PE=") == string_view::npos && pipes < 2;
    case header_format::spd:
        // Exclude NCBI and UniProt prefixes from the generic two-pipe SPD match.
        // Otherwise SPD could win a format-detection tie because it appears first.
        return pipes == 2 && h.find('=') == string_view::npos && !h.starts_with("gi|") &&
               !h.starts_with("pdb|") && !h.starts_with("ref|") && !h.starts_with("sp|") &&
               !h.starts_with("tr|");
    case header_format::ncbi_gi:
        return h.starts_with("gi|");
    case header_format::ncbi_pdb:
        return h.starts_with("pdb|");
    case header_format::refseq:
        return h.starts_with("ref|") || (looks_like_refseq_accession(h) && !has_equals_tag(h));
    case header_format::ncbi:
        return has_trailing_value_bracket(h) && !has_equals_tag(h) && !h.starts_with("sp|") &&
               !h.starts_with("tr|") && !h.starts_with("gi|") && !h.starts_with("pdb|") &&
               !h.starts_with("ref|");
    case header_format::genbank:
        return has_bracket_equals(h);
    case header_format::gisaid: {
        if (pipes < 6)
            return false;
        if (h.empty() || !(h[0] == 'A' || h[0] == 'B'))
            return false;
        if (h.size() < 2 || h[1] != '/')
            return false;
        const std::size_t p1 = h.find('|');
        const std::size_t p2 = (p1 == string_view::npos) ? p1 : h.find('|', p1 + 1);
        const std::size_t p3 = (p2 == string_view::npos) ? p2 : h.find('|', p2 + 1);
        if (p3 == string_view::npos)
            return false;
        const std::size_t p4 = h.find('|', p3 + 1);
        const std::size_t field3_end = (p4 == string_view::npos) ? h.size() : p4;
        return is_date_like(h.substr(p3 + 1, field3_end - (p3 + 1)));
    }
    case header_format::imgt:
        return pipes == 14;
    default:
        return false;
    }
}

} // namespace bioseq::io::identifier_parser::detail

// Public parser functions.

export namespace bioseq::io::identifier_parser {

/// Reads one header and sends selected fields to the writer.
/// The caller starts and finishes the tuple and initializes uset destinations.
/// A missing field can leave its destination untouched.
[[nodiscard]] std::expected<void, std::string>
parse_and_route_header(header_format format,
                       std::string_view header,
                       const identifier_routing& routing,
                       identifier_parse_state& state,
                       identifier_writer_reference writer) {
    state.lexer.lex(header);
    detail::route_emitter emit{routing, writer, {}};
    const header_format_definition& def = header_format_definition_for(format);
    switch (format) {
    case header_format::uniprotkb:
        detail::parse_uniprot(header, state, def, emit);
        break;
    case header_format::uniref:
    case header_format::uniparc:
    case header_format::unimes:
        detail::parse_keyed(header, state, def, emit);
        break;
    case header_format::spd:
    case header_format::gisaid:
    case header_format::imgt:
        detail::parse_positional(header, state, def, emit);
        break;
    case header_format::ncbi:
    case header_format::ncbi_gi:
    case header_format::ncbi_pdb:
    case header_format::refseq:
        detail::parse_ncbi(header, state, def, emit);
        break;
    case header_format::genbank:
        detail::parse_genbank(header, state, def, emit);
        break;
    case header_format::plain:
        detail::parse_plain(header, state, def, emit);
        break;
    case header_format::automatic:
    default:
        detail::parse_auto(header, state, def, emit);
        break;
    }
    return {};
}

/// Chooses the format matching the most sampled headers.
/// Ties go to the first format in enum order With no matches, structured headers
/// use generic parsing and, other headers use `plain`. Empty samples return `automatic`.
[[nodiscard]] header_format
detect_header_format(std::span<const std::string_view> samples) noexcept {
    if (samples.empty())
        return header_format::automatic;

    std::array<int, static_cast<std::size_t>(header_format::count)> votes{};
    bool any_structure = false;
    for (const std::string_view h : samples) {
        if (h.find('|') != std::string_view::npos || h.find('=') != std::string_view::npos ||
            h.find('[') != std::string_view::npos)
            any_structure = true;
        // Test  known formats. `automatic` and `plain` are fallbacks.
        for (std::size_t f = static_cast<std::size_t>(header_format::uniprotkb); f < votes.size();
             ++f) {
            if (detail::format_matches(static_cast<header_format>(f), h))
                ++votes[f];
        }
    }
    int best = 0;
    header_format best_format = header_format::plain;
    for (std::size_t f = static_cast<std::size_t>(header_format::uniprotkb); f < votes.size();
         ++f) {
        if (votes[f] > best) { // strict '>' keeps the lowest enum on ties
            best = votes[f];
            best_format = static_cast<header_format>(f);
        }
    }
    if (best == 0)
        return any_structure ? header_format::automatic : header_format::plain;
    return best_format;
}

/// Chooses which header fields populate each destination column.
/// Explicit mappings take priority. Remaining columns can match format fields
/// or hold the whole identifier. Returns an error when a mapping is invalid
/// or the identifier column is ambiguous.
[[nodiscard]] std::expected<identifier_mapping_result, std::string>
resolve_identifier_mapping(header_format requested_format,
                           header_format effective_format,
                           std::span<const column_descriptor> columns,
                           std::span<const user_column_mapping> user_mappings) {
    identifier_mapping_result result;
    identifier_routing& routing = result.routing;
    std::vector<std::uint16_t>& destination_columns = result.slot_to_column; // slot -> col

    const std::size_t column_count = columns.size();
    std::vector<char> is_user_mapped(column_count, 0);
    std::vector<char> is_metadata_claimed(column_count, 0);
    std::vector<destination_slot> column_to_slot(column_count, -1);
    bool has_key_or_position_route = false;

    // Assign one writer slot to each destination column when first needed.
    auto slot_of = [&column_to_slot, &destination_columns](std::size_t i) -> destination_slot {
        if (column_to_slot[i] >= 0)
            return column_to_slot[i];
        const destination_slot s = static_cast<destination_slot>(destination_columns.size());
        destination_columns.push_back(static_cast<std::uint16_t>(i));
        column_to_slot[i] = s;
        return s;
    };
    // Prevent duplicate destinations within one routing rule.
    auto contains_slot = [](const std::vector<destination_slot>& slots,
                            destination_slot s) -> bool {
        return std::find(slots.begin(), slots.end(), s) != slots.end();
    };
    // Merge one destination into the canonical key route.
    auto add_key_route = [&contains_slot, &has_key_or_position_route, &routing](
                             std::string key, destination_slot s) {
        for (identifier_key_route& route : routing.key_routes) {
            if (route.key == key) {
                if (!contains_slot(route.slots, s)) {
                    route.slots.push_back(s);
                    has_key_or_position_route = true;
                }
                return;
            }
        }
        routing.key_routes.push_back(identifier_key_route{
            .key = std::move(key),
            .slots = std::vector<destination_slot>{s},
        });
        has_key_or_position_route = true;
    };
    // Merge one destination into the positional route.
    auto add_position_route = [&contains_slot, &has_key_or_position_route, &routing](
                                  std::uint32_t p, destination_slot s) {
        for (identifier_position_route& route : routing.position_routes) {
            if (route.position == p) {
                if (!contains_slot(route.slots, s)) {
                    route.slots.push_back(s);
                    has_key_or_position_route = true;
                }
                return;
            }
        }
        routing.position_routes.push_back(identifier_position_route{
            .position = p,
            .slots = std::vector<destination_slot>{s},
        });
        has_key_or_position_route = true;
    };
    // Find a column by name, ignoring ASCII case, underscores, and hyphens.
    auto find_column = [&column_count, &columns](std::string_view raw_name) -> std::size_t {
        for (std::size_t i = 0; i < column_count; ++i)
            if (detail::names_match_normalised(columns[i].name, raw_name))
                return i;
        return column_count;
    };

    const header_format_definition& effective_definition =
        header_format_definition_for(effective_format);

    // Explicit user mappings take priority.
    for (const user_column_mapping& user_mapping : user_mappings) {
        const std::size_t i = find_column(user_mapping.column_name);
        if (i == column_count)
            return std::unexpected(
                fmt::format("mapping target column \"{}\" not found", user_mapping.column_name));
        if (columns[i].is_identity_or_generated)
            return std::unexpected(
                fmt::format("mapping target column \"{}\" is an identity/generated column "
                            "and cannot be populated by bioseq.load",
                            user_mapping.column_name));
        is_user_mapped[i] = 1;
        const destination_slot slot = slot_of(i);

        const std::string_view ds = detail::trim_spaces(user_mapping.data_source);
        std::uint32_t index = 0;
        const auto [ptr, ec] = std::from_chars(ds.data(), ds.data() + ds.size(), index);
        const bool is_index = !ds.empty() && ec == std::errc{} && ptr == ds.data() + ds.size();
        if (is_index) {
            add_position_route(index, slot);
        } else if (effective_format == header_format::automatic) {
            add_key_route(detail::normalise_to_string(user_mapping.data_source), slot);
        } else {
            const header_field_definition* fd =
                detail::find_field_for_name(effective_definition, user_mapping.data_source);
            if (!fd)
                return std::unexpected(fmt::format("\"{}\" is not a field of format {}",
                                                   user_mapping.data_source,
                                                   effective_definition.sql_name));
            if (fd->origin == field_origin::positional)
                add_position_route(fd->position, slot);
            else
                add_key_route(std::string(fd->normalised_key), slot);
        }
    }

    // Match remaining columns to fields of the selected  format.
    if (effective_format != header_format::automatic && effective_format != header_format::plain) {
        for (std::size_t i = 0; i < column_count; ++i) {
            if (is_user_mapped[i])
                continue;
            const header_field_definition* fd =
                detail::find_field_for_name(effective_definition, columns[i].name);
            if (!fd)
                continue;
            const destination_slot s = slot_of(i);
            is_metadata_claimed[i] = 1;
            if (fd->origin == field_origin::positional)
                add_position_route(fd->position, s);
            else
                add_key_route(std::string(fd->normalised_key), s);
        }
    }

    // Choose an identifier column from the text columns still unclaimed.
    boost::container::small_vector<std::size_t, 4> text_candidates, primary_key_candidates,
        name_candidates;
    for (std::size_t i = 0; i < column_count; ++i) {
        if (!columns[i].is_text || is_user_mapped[i] || is_metadata_claimed[i])
            continue;
        text_candidates.push_back(i);
        if (columns[i].is_primary_key)
            primary_key_candidates.push_back(i);
        if (detail::names_match_normalised(columns[i].name, "id") ||
            detail::names_match_normalised(columns[i].name, "identifier") ||
            detail::names_match_normalised(columns[i].name, "header") ||
            detail::names_match_normalised(columns[i].name, "name"))
            name_candidates.push_back(i);
    }
    std::size_t identifier_column = column_count; // no identifier column selected
    if (primary_key_candidates.size() == 1)
        identifier_column = primary_key_candidates[0];
    else if (name_candidates.size() == 1)
        identifier_column = name_candidates[0];
    else if (text_candidates.size() == 1)
        identifier_column = text_candidates[0];
    else if (text_candidates.size() > 1)
        return std::unexpected(
            fmt::format("{} unmapped text columns and no single PRIMARY KEY / identifier-name "
                        "alias; pass columns => ARRAY[(...)] to disambiguate",
                        text_candidates.size()));
    if (identifier_column != column_count)
        routing.identifier_slots.push_back(slot_of(identifier_column));

    // AUTO also matches remaining column names to metadata keys.
    if (requested_format == header_format::automatic) {
        for (std::size_t i = 0; i < column_count; ++i) {
            if (is_user_mapped[i] || i == identifier_column || is_metadata_claimed[i])
                continue;
            add_key_route(detail::normalise_to_string(columns[i].name), slot_of(i));
        }
    }

    // Decide whether to remove metadata from the identifier after all routes are known.
    routing.strip_metadata = (requested_format == header_format::automatic)
                                 ? has_key_or_position_route
                                 : (effective_definition.metadata != metadata_style::none);
    routing.destination_count = destination_columns.size();
    return result;
}

} // namespace bioseq::io::identifier_parser
