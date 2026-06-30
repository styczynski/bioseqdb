// bioseq/io/identifier_parser/identifier_parser.cppm
//
// DOC0295 §4-§7 — NO_POSTGRES FASTA/FASTQ header (identifier line) parser. One
// SIMD pass (`scan_for_each_delim4`) lexes the structural delimiters; per-format
// byte-walkers extract values; a runtime routing table fans each value to zero or
// more destination "slots"; values are handed to a concept-constrained `Writer`
// (the PG side supplies the coercion). The parser carries NO Postgres types and
// is NOT templated on alphabet / storage-width / file-type (DOC0271 / §3): the
// per-format extractors are instantiated once, against the single non-template
// `identifier_writer_ref` adapter, so they add zero entries to load.cppm's
// alphabet×width×file_type fan-out.
module;

#include "common.h"            // fmt (DOC0145)

#include <boost/container/small_vector.hpp>

#include <algorithm>
#include <array>
#include <charconv>
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

import bioseq.common.mlogging;
import bioseq.io.simd_kernels;      // scan_for_each_delim4 (one SIMD pass per header)
import bioseq.io.file_utils;        // bioseq::io::detail::to_ascii_lower (shared ASCII fold)
import bioseq.io.identifier_formats;

static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::io::idparse {

// =====================================================================
// §4.3 — slot type, user mapping, column descriptor, routing, result
// =====================================================================

using dest_slot_t = std::int32_t;   // one slot int type everywhere

/// One entry of the SQL `columns => ARRAY[(column_name, data_source)]` argument.
/// The PG side flattens the composite into these borrowed `string_view`s; they
/// stay valid until `resolve_identifier_mapping` returns (§4.3 lifetime contract).
struct user_column_mapping {
    std::string_view column_name;
    std::string_view data_source;
};

/// One non-seq column as the PG side describes it to the resolver. NO Postgres
/// types — `Form_pg_attribute` is flattened into these plain fields.
struct column_descriptor {
    std::string_view name;        // raw column name
    bool             is_text;     // TEXT/VARCHAR/BPCHAR (fast path, no input fn)
    bool             is_primary_key;
    bool             not_null;
    bool             has_default;
    bool             is_identity_or_generated;
};

/// Routes the parser produces -> destination "slots". A slot is a dense index
/// into the PG-side coercion table. One field may route to MANY slots (D5).
/// Route counts are tiny and resolved once per load, so vector-backed route
/// lists avoid exposing Boost flat_map internals through this module BMI.
struct identifier_key_route {
    std::string key;
    std::vector<dest_slot_t> slots;
};

struct identifier_pos_route {
    std::uint32_t position;
    std::vector<dest_slot_t> slots;
};

struct identifier_routing {
    std::vector<identifier_key_route> key_routes;
    std::vector<identifier_pos_route> pos_routes;
    std::vector<dest_slot_t> identifier_slots;
    std::size_t dest_count     = 0;     // number of distinct slots
    bool        strip_metadata = false; // D7: strip [k=v]/k=v from identifier
};

/// What `resolve_identifier_mapping` returns. `slot_to_col` maps each slot to the
/// caller's column index (an index into the `columns` span passed in).
struct identifier_mapping_result {
    identifier_routing         routing;
    std::vector<std::uint16_t> slot_to_col;   // slot -> index into input columns
};

// =====================================================================
// §4.4 — the Writer concept (D2) + type-erased ref adapter
// =====================================================================

/// The postgres-agnostic sink the parser drives. The concrete implementation
/// (`pg_identifier_writer` in load.cppm) coerces each raw byte range into the
/// destination column's type and fills the virtual-tuple slot. Modelled on
/// `bioseq::indexing::is_index_for` / the `io_sink` concept (member-call style).
template <typename W>
concept is_identifier_dest_writer = requires(
        W& w, const W& cw, dest_slot_t slot, const char* data, std::int32_t len) {
    { w.begin_tuple() }              -> std::same_as<void>;
    { w.set_field(slot, data, len) } -> std::same_as<void>;  // universal coercion
    { w.set_null(slot) }             -> std::same_as<void>;
    { w.end_tuple() }                -> std::same_as<void>;
    { w.finish() }                   -> std::same_as<void>;
    { cw.dest_count() }              -> std::convertible_to<std::size_t>;
};

/// Reference/no-op writer used by the compile-time concept check and NO_POSTGRES
/// parser unit tests.
struct example_identifier_dest_writer {
    std::size_t n = 0;
    void begin_tuple() noexcept {}
    void set_field(dest_slot_t, const char*, std::int32_t) noexcept {}
    void set_null(dest_slot_t) noexcept {}
    void end_tuple() noexcept {}
    void finish() noexcept {}
    [[nodiscard]] std::size_t dest_count() const noexcept { return n; }
};
static_assert(is_identifier_dest_writer<example_identifier_dest_writer>);

/// Non-template adapter used by the parser hot path. Built once in the PG caller
/// from a concept-satisfying writer; the parser takes only this erased ref, so
/// the per-format extractors are not instantiated per writer type (the
/// load-bearing template-explosion invariant of §3/§4.4).
struct identifier_writer_ref {
    void* self = nullptr;
    void (*begin_tuple_fn)(void*) = nullptr;
    void (*set_field_fn)(void*, dest_slot_t, const char*, std::int32_t) = nullptr;
    void (*set_null_fn)(void*, dest_slot_t) = nullptr;
    void (*end_tuple_fn)(void*) = nullptr;
    void (*finish_fn)(void*) = nullptr;
    std::size_t (*dest_count_fn)(const void*) = nullptr;

    void begin_tuple() const { begin_tuple_fn(self); }
    void set_field(dest_slot_t s, const char* p, std::int32_t n) const {
        set_field_fn(self, s, p, n);
    }
    void set_null(dest_slot_t s) const { set_null_fn(self, s); }
    void end_tuple() const { end_tuple_fn(self); }
    void finish() const { finish_fn(self); }
    [[nodiscard]] std::size_t dest_count() const { return dest_count_fn(self); }
};

template <is_identifier_dest_writer W>
[[nodiscard]] identifier_writer_ref make_identifier_writer_ref(W& w) noexcept {
    return identifier_writer_ref{
        .self = &w,
        .begin_tuple_fn = [](void* p) { static_cast<W*>(p)->begin_tuple(); },
        .set_field_fn = [](void* p, dest_slot_t s, const char* d, std::int32_t n) {
            static_cast<W*>(p)->set_field(s, d, n);
        },
        .set_null_fn = [](void* p, dest_slot_t s) { static_cast<W*>(p)->set_null(s); },
        .end_tuple_fn = [](void* p) { static_cast<W*>(p)->end_tuple(); },
        .finish_fn = [](void* p) { static_cast<W*>(p)->finish(); },
        .dest_count_fn = [](const void* p) {
            return static_cast<const W*>(p)->dest_count();
        },
    };
}

// =====================================================================
// §5.2 — lexer output + reusable per-record parser scratch
// =====================================================================

enum class delim_kind : std::uint8_t { PIPE, EQUALS, LBRACKET, RBRACKET };
struct delim_token { std::uint32_t offset; delim_kind kind; };

/// One SIMD pass collecting every structural delimiter, ascending. Headers are
/// short, so the inline `small_vector` buffer never allocates in the common case.
struct header_lexer {
    boost::container::small_vector<delim_token, 48> tokens;

    void lex(std::string_view header) {
        tokens.clear();
        struct lex_ctx { header_lexer* self; const char* base; };
        lex_ctx ctx{this, header.data()};
        bioseq::io::simd::scan_for_each_delim4(
            '|', '=', '[', ']',
            reinterpret_cast<const std::uint8_t*>(header.data()), header.size(),
            &ctx,
            [](void* p, std::size_t offset) {
                auto* c = static_cast<lex_ctx*>(p);
                const char ch = c->base[offset];
                const delim_kind k =
                    ch == '|' ? delim_kind::PIPE
                  : ch == '=' ? delim_kind::EQUALS
                  : ch == '[' ? delim_kind::LBRACKET
                  :             delim_kind::RBRACKET;
                c->self->tokens.push_back(
                    {static_cast<std::uint32_t>(offset), k});
            });
    }
};

/// Reusable parser scratch owned by the caller (`LoadContext` on the PG side).
/// No TLS and no per-record heap allocation in steady state.
struct identifier_parse_state {
    header_lexer lexer;
    std::string  strip_scratch;
};

}  // namespace bioseq::io::idparse

// =====================================================================
// Internal helpers (module-local; not exported). §5.4 contracts.
// =====================================================================

namespace bioseq::io::idparse::detail {

using std::string_view;
namespace bc = boost::container;

[[nodiscard]] constexpr bool is_key_char(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9');
}

/// Compare two raw names normalisation-insensitively (lower-case + drop '_'/'-')
/// with no scratch buffer. Reuses the single ASCII-fold rule (`to_ascii_lower`).
[[nodiscard]] constexpr bool names_match_normalised(
        string_view a, string_view b) noexcept {
    std::size_t i = 0, j = 0;
    auto advance = [](string_view s, std::size_t& k) -> int {
        while (k < s.size()) {
            const char c = s[k++];
            if (c == '_' || c == '-') continue;
            return static_cast<unsigned char>(bioseq::io::detail::to_ascii_lower(c));
        }
        return -1;
    };
    for (;;) {
        const int ca = advance(a, i);
        const int cb = advance(b, j);
        if (ca != cb) return false;
        if (ca == -1) return true;
    }
}

/// Lower-case + drop '_'/'-' into a fresh std::string (resolution-time only).
[[nodiscard]] inline std::string normalise_to_string(string_view in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '_' || c == '-') continue;
        out.push_back(bioseq::io::detail::to_ascii_lower(c));
    }
    return out;
}

[[nodiscard]] constexpr string_view ltrim_spaces(string_view s) noexcept {
    std::size_t b = 0;
    while (b < s.size() && s[b] == ' ') ++b;
    return s.substr(b);
}
[[nodiscard]] constexpr string_view rtrim_spaces(string_view s) noexcept {
    std::size_t e = s.size();
    while (e > 0 && s[e - 1] == ' ') --e;
    return s.substr(0, e);
}
[[nodiscard]] constexpr string_view trim_spaces(string_view s) noexcept {
    return rtrim_spaces(ltrim_spaces(s));
}

/// §5.4 — fields between consecutive `delim` delimiters; virtual delimiters at
/// -1 and len. Empty fields are preserved. PIPE offsets come from the lexer; any
/// other delimiter (no current format uses one) falls back to a scalar scan.
[[nodiscard]] inline bc::small_vector<string_view, 16> positional_split_on(
        char delim, string_view header, const header_lexer& lex) {
    bc::small_vector<string_view, 16> out;
    std::size_t start = 0;
    if (delim == '|') {
        for (const auto& t : lex.tokens) {
            if (t.kind == delim_kind::PIPE) {
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

/// §5.4 — split at the first ' '. `tail` is empty if there is no space.
[[nodiscard]] inline std::pair<string_view, string_view> split_first_space(
        string_view s) {
    const std::size_t pos = bioseq::io::simd::scan_first_byte(
        ' ', reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    if (pos >= s.size()) return {s, string_view{}};
    return {s.substr(0, pos), s.substr(pos + 1)};
}

/// Pair each LBRACKET with the next RBRACKET -> (lb, rb) offsets.
[[nodiscard]] inline bc::small_vector<std::pair<std::uint32_t, std::uint32_t>, 8>
bracket_spans(const header_lexer& lex) {
    bc::small_vector<std::pair<std::uint32_t, std::uint32_t>, 8> out;
    std::optional<std::uint32_t> open;
    for (const auto& t : lex.tokens) {
        if (t.kind == delim_kind::LBRACKET) open = t.offset;
        else if (t.kind == delim_kind::RBRACKET && open) {
            out.push_back({*open, t.offset});
            open.reset();
        }
    }
    return out;
}

[[nodiscard]] inline bool offset_in_any_bracket(
        const bc::small_vector<std::pair<std::uint32_t, std::uint32_t>, 8>& spans,
        std::uint32_t e) {
    for (const auto& [lb, rb] : spans)
        if (lb < e && e < rb) return true;
    return false;
}

struct accepted_eq {
    std::uint32_t            ks;     // key-run start
    std::uint32_t            eq;     // '=' offset
    const header_field_def*  field;  // matched EQUALS_TAG field (fixed) or nullptr (AUTO)
};

/// §5.3 — collect every accepted KEY=VALUE tag whose '=' falls in [begin,end) and
/// is not in-bracket: non-empty `[A-Za-z0-9]` key run, boundary char before the
/// key (start / space / '|'), and (fixed formats) a whitelist match against the
/// format's EQUALS_TAG fields. Ascending by offset.
[[nodiscard]] inline bc::small_vector<accepted_eq, 16> collect_accepted_equals(
        string_view header, const header_lexer& lex,
        std::uint32_t begin, std::uint32_t end,
        const header_format_def& def, bool accept_any) {
    const auto spans = bracket_spans(lex);
    bc::small_vector<accepted_eq, 16> out;
    for (const auto& t : lex.tokens) {
        if (t.kind != delim_kind::EQUALS) continue;
        const std::uint32_t e = t.offset;
        if (e < begin || e >= end) continue;
        if (offset_in_any_bracket(spans, e)) continue;
        if (e == 0) continue;
        std::uint32_t ks = e;
        while (ks > 0 && is_key_char(header[ks - 1])) --ks;
        if (ks == e) continue;  // empty key run
        const bool boundary =
            (ks == 0) || header[ks - 1] == ' ' || header[ks - 1] == '|';
        if (!boundary) continue;
        const header_field_def* field = nullptr;
        if (!accept_any) {
            const string_view key = header.substr(ks, e - ks);
            for (const auto& fd : def.fields) {
                if (fd.origin != field_origin::EQUALS_TAG) continue;
                if (names_match_normalised(key, fd.canonical) ||
                    (!fd.token.empty() && names_match_normalised(key, fd.token))) {
                    field = &fd;
                    break;
                }
            }
            if (field == nullptr) continue;  // not whitelisted
        }
        out.push_back({ks, e, field});
    }
    return out;
}

struct equals_tag {
    string_view             key;
    string_view             value;   // right-trimmed
    const header_field_def* field;
};

/// §5.4 `equals_tags` — accepted EQUALS tags with right-trimmed values. Each
/// value runs from just after its '=' to the next accepted tag's key start (or
/// `end`), then trailing spaces are removed.
[[nodiscard]] inline bc::small_vector<equals_tag, 16> equals_tags(
        string_view header, const header_lexer& lex,
        std::uint32_t begin, std::uint32_t end,
        const header_format_def& def, bool accept_any) {
    const auto acc =
        collect_accepted_equals(header, lex, begin, end, def, accept_any);
    bc::small_vector<equals_tag, 16> out;
    for (std::size_t i = 0; i < acc.size(); ++i) {
        const std::uint32_t vstart = acc[i].eq + 1;
        const std::uint32_t vend = (i + 1 < acc.size()) ? acc[i + 1].ks : end;
        const string_view value = rtrim_spaces(
            header.substr(vstart, vend > vstart ? (vend - vstart) : 0));
        const string_view key = header.substr(acc[i].ks, acc[i].eq - acc[i].ks);
        out.push_back({key, value, acc[i].field});
    }
    return out;
}

/// §5.4 `first_tag_offset` — min key-start of an accepted EQUALS tag or LBRACKET
/// within [begin,end), else `end`. Bounds the description / protein-name region.
[[nodiscard]] inline std::uint32_t first_tag_offset(
        string_view header, const header_lexer& lex,
        std::uint32_t begin, std::uint32_t end,
        const header_format_def& def, bool accept_any) {
    std::uint32_t first = end;
    const auto acc =
        collect_accepted_equals(header, lex, begin, end, def, accept_any);
    if (!acc.empty()) first = std::min(first, acc[0].ks);
    for (const auto& [lb, rb] : bracket_spans(lex)) {
        (void) rb;
        if (lb >= begin && lb < end) first = std::min(first, lb);
    }
    return first;
}

/// §5.4 `bracket_tags` — the `[KEY=VALUE]` spans (exactly one in-span '=').
[[nodiscard]] inline bc::small_vector<std::pair<string_view, string_view>, 8>
bracket_tags(string_view header, const header_lexer& lex) {
    bc::small_vector<std::pair<string_view, string_view>, 8> out;
    for (const auto& [lb, rb] : bracket_spans(lex)) {
        int eq_count = 0;
        std::uint32_t epos = 0;
        for (const auto& t : lex.tokens)
            if (t.kind == delim_kind::EQUALS && t.offset > lb && t.offset < rb) {
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

/// §5.4 `trailing_bracket_value` — trimmed contents of the LAST `[...]` span that
/// contains no '=' (the NCBI/RefSeq value-only `[organism]` convention).
[[nodiscard]] inline string_view trailing_bracket_value(
        string_view header, const header_lexer& lex) {
    string_view result;
    for (const auto& [lb, rb] : bracket_spans(lex)) {
        bool has_eq = false;
        for (const auto& t : lex.tokens)
            if (t.kind == delim_kind::EQUALS && t.offset > lb && t.offset < rb) {
                has_eq = true;
                break;
            }
        if (!has_eq) result = trim_spaces(header.substr(lb + 1, rb - (lb + 1)));
    }
    return result;
}

/// §5.4 `trailing_bracket_start` — `lb` of that same last-no-'=' span, or npos.
[[nodiscard]] inline std::size_t trailing_bracket_start(
        string_view header, const header_lexer& lex) {
    std::size_t result = string_view::npos;
    for (const auto& [lb, rb] : bracket_spans(lex)) {
        (void) header;
        bool has_eq = false;
        for (const auto& t : lex.tokens)
            if (t.kind == delim_kind::EQUALS && t.offset > lb && t.offset < rb) {
                has_eq = true;
                break;
            }
        if (!has_eq) result = lb;
    }
    return result;
}

/// §5.4 `strip_tags` — header with every accepted EQUALS tag and every bracket
/// span removed, runs of >1 space collapsed to one and ends trimmed, written into
/// the caller-owned `scratch`. The D7 "metadata-stripped identifier".
[[nodiscard]] inline string_view strip_tags(
        string_view header, const header_lexer& lex, std::string& scratch,
        const header_format_def& def, bool accept_any) {
    bc::small_vector<std::pair<std::uint32_t, std::uint32_t>, 16> remove;  // [start,end)
    const auto acc = collect_accepted_equals(
        header, lex, 0, static_cast<std::uint32_t>(header.size()), def, accept_any);
    for (std::size_t i = 0; i < acc.size(); ++i) {
        const std::uint32_t start = acc[i].ks;
        const std::uint32_t end = (i + 1 < acc.size())
            ? acc[i + 1].ks : static_cast<std::uint32_t>(header.size());
        remove.push_back({start, end});
    }
    for (const auto& [lb, rb] : bracket_spans(lex))
        remove.push_back({lb, rb + 1});
    std::sort(remove.begin(), remove.end());

    scratch.clear();
    auto append_collapsed = [&](string_view chunk) {
        for (char c : chunk) {
            if (c == ' ') {
                if (!scratch.empty() && scratch.back() != ' ') scratch.push_back(' ');
            } else {
                scratch.push_back(c);
            }
        }
    };
    std::size_t cursor = 0;
    for (const auto& [start, end] : remove) {
        if (start > cursor) append_collapsed(header.substr(cursor, start - cursor));
        if (end > cursor) cursor = end;
    }
    if (cursor < header.size()) append_collapsed(header.substr(cursor));
    while (!scratch.empty() && scratch.back() == ' ') scratch.pop_back();
    return string_view{scratch};
}

// ---------------------------------------------------------------------
// Route emitter — the §6 `Emit` that consults the routing table and writes.
// ---------------------------------------------------------------------

[[nodiscard]] inline const std::vector<dest_slot_t>* find_key_slots(
        const identifier_routing& routing, string_view key) noexcept {
    for (const auto& route : routing.key_routes) {
        if (string_view{route.key} == key) return &route.slots;
    }
    return nullptr;
}

[[nodiscard]] inline const std::vector<dest_slot_t>* find_pos_slots(
        const identifier_routing& routing, std::uint32_t position) noexcept {
    for (const auto& route : routing.pos_routes) {
        if (route.position == position) return &route.slots;
    }
    return nullptr;
}

struct route_emitter {
    const identifier_routing& routing;
    identifier_writer_ref     writer;
    std::array<char, 256>     key_buf{};   // AUTO key normalisation scratch

    void write_or_null(dest_slot_t slot, string_view value) const {
        if (value.empty()) writer.set_null(slot);
        else writer.set_field(slot, value.data(),
                              static_cast<std::int32_t>(value.size()));
    }
    /// Route by an already-normalised key (fixed-format `norm_key` or a literal).
    void keyed(string_view norm_key, string_view value) const {
        if (const auto* slots = find_key_slots(routing, norm_key)) {
            for (dest_slot_t s : *slots) write_or_null(s, value);
        }
    }
    /// Route by a raw header key (AUTO / bracket tags): normalise then route.
    void keyed_raw(string_view raw_key, string_view value) {
        if (raw_key.size() <= key_buf.size()) {
            keyed(normalise_name(raw_key, key_buf), value);
        } else {
            const std::string nk = normalise_to_string(raw_key);
            keyed(nk, value);
        }
    }
    void positional(std::uint32_t idx, string_view value) const {
        if (const auto* slots = find_pos_slots(routing, idx)) {
            for (dest_slot_t s : *slots) write_or_null(s, value);
        }
    }
    void identifier(string_view value) const {
        for (dest_slot_t s : routing.identifier_slots) write_or_null(s, value);
    }
};

/// Shared identifier emission: the (optionally metadata-stripped) leading text.
inline void emit_identifier(
        string_view header, identifier_parse_state& st,
        const header_format_def& def, bool accept_any,
        const identifier_routing& routing, route_emitter& emit) {
    if (routing.identifier_slots.empty()) return;
    if (routing.strip_metadata)
        emit.identifier(strip_tags(header, st.lexer, st.strip_scratch, def, accept_any));
    else
        emit.identifier(header);
}

[[nodiscard]] inline const header_field_def* find_field_for_name(
        const header_format_def& def, string_view name) {
    for (const auto& fd : def.fields) {
        if (names_match_normalised(name, fd.canonical)) return &fd;
        if (!fd.token.empty() && names_match_normalised(name, fd.token)) return &fd;
        for (const string_view alias : fd.aliases)
            if (names_match_normalised(name, alias)) return &fd;
    }
    return nullptr;
}

// ---------------------------------------------------------------------
// §6 — per-format extractors
// ---------------------------------------------------------------------

/// §6.1 — UniProtKB: `db|accession|entry` block + EQUALS tags.
inline void parse_uniprot(
        string_view header, identifier_parse_state& st,
        const header_format_def& def, route_emitter& emit) {
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
    string_view after{header.data() + header.size(), 0};  // empty by default
    if (parts.size() >= 3) {
        const auto [entry, tail] = split_first_space(parts[2]);
        emit.positional(2, entry);
        emit.keyed("entryname", entry);
        const std::size_t us = entry.find('_');
        if (us != string_view::npos &&
            entry.find('_', us + 1) == string_view::npos) {
            emit.keyed("geneid", entry.substr(0, us));
            emit.keyed("taxon", entry.substr(us + 1));
        }
        after = tail;
    }
    const std::uint32_t begin =
        static_cast<std::uint32_t>(after.data() - header.data());
    const std::uint32_t end = begin + static_cast<std::uint32_t>(after.size());
    const auto tags = equals_tags(header, lex, begin, end, def, /*accept_any=*/false);
    const std::uint32_t desc_end =
        first_tag_offset(header, lex, begin, end, def, /*accept_any=*/false);
    const string_view protein_name =
        rtrim_spaces(header.substr(begin, desc_end - begin));
    emit.keyed("proteinname", protein_name);
    for (const auto& tag : tags)
        if (tag.field) emit.keyed(tag.field->norm_key, tag.value);
    emit_identifier(header, st, def, /*accept_any=*/false, emit.routing, emit);
}

/// §6.1b — first-token id + EQUALS tags (UniRef, UniParc, UniMes).
inline void parse_keyed(
        string_view header, identifier_parse_state& st,
        const header_format_def& def, route_emitter& emit) {
    const header_lexer& lex = st.lexer;
    const auto [id, after] = split_first_space(header);
    emit.positional(0, id);
    emit.keyed("id", id);
    const std::uint32_t begin =
        static_cast<std::uint32_t>(after.data() - header.data());
    const std::uint32_t end = begin + static_cast<std::uint32_t>(after.size());
    const auto tags = equals_tags(header, lex, begin, end, def, /*accept_any=*/false);

    const header_field_def* fd_desc = nullptr;
    for (const auto& fd : def.fields)
        if (fd.origin == field_origin::DESCRIPTION) { fd_desc = &fd; break; }
    if (fd_desc) {
        const std::uint32_t desc_end =
            first_tag_offset(header, lex, begin, end, def, /*accept_any=*/false);
        emit.keyed(fd_desc->norm_key,
                   rtrim_spaces(header.substr(begin, desc_end - begin)));
    }
    for (const auto& tag : tags)
        if (tag.field) emit.keyed(tag.field->norm_key, tag.value);
    emit_identifier(header, st, def, /*accept_any=*/false, emit.routing, emit);
}

/// §6.2 — pure positional formats (SPD, GISAID, IMGT).
inline void parse_positional(
        string_view header, identifier_parse_state& st,
        const header_format_def& def, route_emitter& emit) {
    const header_lexer& lex = st.lexer;
    const auto parts = positional_split_on(def.primary_delim, header, lex);
    for (std::size_t i = 0; i < parts.size(); ++i)
        emit.positional(static_cast<std::uint32_t>(i), trim_spaces(parts[i]));
    for (const auto& fd : def.fields)
        if (fd.origin == field_origin::POSITIONAL &&
            fd.position < parts.size())
            emit.keyed(fd.norm_key, trim_spaces(parts[fd.position]));
    // SPD: split pos1 (gene) on '_' into gene_id / taxon.
    if (def.id == header_format::SPD && parts.size() > 1) {
        const string_view gene = trim_spaces(parts[1]);
        const std::size_t us = gene.find('_');
        if (us != string_view::npos) {
            emit.keyed("geneid", gene.substr(0, us));
            emit.keyed("taxon", gene.substr(us + 1));
        }
    }
    emit_identifier(header, st, def, /*accept_any=*/false, emit.routing, emit);
}

/// §6.2b — NCBI family (NCBI generic, gi, pdb, RefSeq): id block + free-text
/// description + trailing value-only `[organism]`.
inline void parse_ncbi(
        string_view header, identifier_parse_state& st,
        const header_format_def& def, route_emitter& emit) {
    const header_lexer& lex = st.lexer;
    string_view rest;
    std::uint32_t rest_begin = static_cast<std::uint32_t>(header.size());

    if (def.primary_delim == '|') {
        const auto parts = positional_split_on('|', header, lex);
        for (std::size_t i = 0; i < parts.size(); ++i)
            emit.positional(static_cast<std::uint32_t>(i), trim_spaces(parts[i]));
        for (const auto& fd : def.fields)
            if (fd.origin == field_origin::POSITIONAL && fd.position < parts.size())
                emit.keyed(fd.norm_key, trim_spaces(parts[fd.position]));
        const std::size_t idblk = def.id_block_field_count;
        if (def.id == header_format::NCBI_PDB && idblk < parts.size()) {
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
    (void) rest;

    const std::size_t bstart = trailing_bracket_start(header, lex);
    const std::uint32_t desc_end =
        (bstart != string_view::npos && bstart >= rest_begin)
            ? static_cast<std::uint32_t>(bstart)
            : static_cast<std::uint32_t>(header.size());
    emit.keyed("description",
               rtrim_spaces(header.substr(rest_begin, desc_end - rest_begin)));
    const string_view org = trailing_bracket_value(header, lex);
    if (!org.empty()) {
        emit.keyed("organism", org);
        emit.keyed("taxon", org);
    }
    emit_identifier(header, st, def, /*accept_any=*/false, emit.routing, emit);
}

/// §6.3 — GenBank `[k=v]` bracket tags.
inline void parse_genbank(
        string_view header, identifier_parse_state& st,
        const header_format_def& def, route_emitter& emit) {
    const header_lexer& lex = st.lexer;
    const auto [id, after] = split_first_space(header);
    (void) after;
    emit.positional(0, id);
    for (const auto& [key, value] : bracket_tags(header, lex))
        emit.keyed_raw(key, value);
    emit_identifier(header, st, def, /*accept_any=*/false, emit.routing, emit);
}

/// §6.4 — PLAIN / bare accession: identifier only.
inline void parse_plain(
        string_view header, identifier_parse_state& st,
        const header_format_def& def, route_emitter& emit) {
    emit_identifier(header, st, def, /*accept_any=*/false, emit.routing, emit);
}

/// §6.5 — AUTO generic path (no fixed format chosen).
inline void parse_auto(
        string_view header, identifier_parse_state& st,
        const header_format_def& def, route_emitter& emit) {
    const header_lexer& lex = st.lexer;
    const auto parts = positional_split_on('|', header, lex);
    for (std::size_t i = 0; i < parts.size(); ++i)
        emit.positional(static_cast<std::uint32_t>(i), trim_spaces(parts[i]));
    const auto tags = equals_tags(
        header, lex, 0, static_cast<std::uint32_t>(header.size()),
        def, /*accept_any=*/true);
    for (const auto& tag : tags) emit.keyed_raw(tag.key, tag.value);
    for (const auto& [key, value] : bracket_tags(header, lex))
        emit.keyed_raw(key, value);
    const string_view org = trailing_bracket_value(header, lex);
    if (!org.empty()) {
        emit.keyed("organism", org);
        emit.keyed("taxon", org);
    }
    emit_identifier(header, st, def, /*accept_any=*/true, emit.routing, emit);
}

// ---------------------------------------------------------------------
// §9.5 — detection predicates
// ---------------------------------------------------------------------

[[nodiscard]] inline std::size_t pipe_count(string_view h) {
    std::size_t n = 0;
    for (char c : h) if (c == '|') ++n;
    return n;
}

/// §8 GISAID `is_date_like` — digits with exactly two '/'-or-'-' separators and
/// three non-empty digit groups (detection heuristic only; coercion is date_in).
[[nodiscard]] inline bool is_date_like(string_view s) {
    if (s.empty()) return false;
    int separators = 0;
    std::size_t group_len = 0;
    int groups = 0;
    for (char c : s) {
        if (c == '/' || c == '-') {
            if (group_len == 0) return false;  // empty group
            ++separators;
            ++groups;
            group_len = 0;
            if (separators > 2) return false;
        } else if (c >= '0' && c <= '9') {
            ++group_len;
        } else {
            return false;  // non-date byte
        }
    }
    if (group_len == 0) return false;  // trailing separator
    ++groups;
    return separators == 2 && groups == 3;
}

[[nodiscard]] inline bool has_equals_tag(string_view h) {
    // any boundary-anchored KEY= (cheap: ' X=' or leading 'X=')
    for (std::size_t i = 0; i < h.size(); ++i)
        if (h[i] == '=' && i > 0 && is_key_char(h[i - 1])) {
            std::size_t k = i;
            while (k > 0 && is_key_char(h[k - 1])) --k;
            if (k == 0 || h[k - 1] == ' ' || h[k - 1] == '|') return true;
        }
    return false;
}

[[nodiscard]] inline bool has_bracket_equals(string_view h) {
    std::size_t lb = h.find('[');
    while (lb != string_view::npos) {
        const std::size_t rb = h.find(']', lb + 1);
        if (rb == string_view::npos) break;
        if (h.substr(lb + 1, rb - lb - 1).find('=') != string_view::npos) return true;
        lb = h.find('[', rb + 1);
    }
    return false;
}

[[nodiscard]] inline bool has_trailing_value_bracket(string_view h) {
    const std::size_t rb = h.rfind(']');
    if (rb == string_view::npos) return false;
    const std::size_t lb = h.rfind('[', rb);
    if (lb == string_view::npos) return false;
    return h.substr(lb + 1, rb - lb - 1).find('=') == string_view::npos;
}

[[nodiscard]] inline bool looks_like_refseq_accession(string_view h) {
    const auto [first, rest] = split_first_space(h);
    (void) rest;
    // [A-Z]{2}_ prefix, no '|' in the first token.
    if (first.size() < 3) return false;
    if (first.find('|') != string_view::npos) return false;
    return first[0] >= 'A' && first[0] <= 'Z'
        && first[1] >= 'A' && first[1] <= 'Z'
        && first[2] == '_';
}

/// Run a single format's §8 detection predicate over one header.
[[nodiscard]] inline bool format_matches(header_format f, string_view h) {
    const std::size_t pipes = pipe_count(h);
    switch (f) {
        case header_format::UNIPROTKB:
            return pipes >= 2 && has_equals_tag(h) &&
                (h.find("OS=") != string_view::npos ||
                 h.find("OX=") != string_view::npos ||
                 h.find("GN=") != string_view::npos ||
                 h.find("PE=") != string_view::npos ||
                 h.find("SV=") != string_view::npos);
        case header_format::UNIREF:
            return h.starts_with("UniRef") &&
                   h.find(" Tax=") != string_view::npos &&
                   h.find(" n=") != string_view::npos;
        case header_format::UNIPARC:
            return h.starts_with("UPI") &&
                   (h.find(" status=") != string_view::npos ||
                    h.find("PC=") != string_view::npos);
        case header_format::UNIMES:
            return h.find("OS=") != string_view::npos &&
                   h.find("SV=") != string_view::npos &&
                   h.find("PE=") == string_view::npos &&
                   pipes < 2;
        case header_format::SPD:
            // "exactly 2 '|' and no '='" (§8), but a leading NCBI/UniProt prefix
            // is unambiguously that family — keep SPD's generic heuristic from
            // stealing `gi|N| …` etc. via the enum-order tie-break.
            return pipes == 2 && h.find('=') == string_view::npos &&
                   !h.starts_with("gi|") && !h.starts_with("pdb|") &&
                   !h.starts_with("ref|") && !h.starts_with("sp|") &&
                   !h.starts_with("tr|");
        case header_format::NCBI_GI:
            return h.starts_with("gi|");
        case header_format::NCBI_PDB:
            return h.starts_with("pdb|");
        case header_format::REFSEQ:
            return h.starts_with("ref|") ||
                   (looks_like_refseq_accession(h) && !has_equals_tag(h));
        case header_format::NCBI:
            return has_trailing_value_bracket(h) && !has_equals_tag(h) &&
                   !h.starts_with("sp|") && !h.starts_with("tr|") &&
                   !h.starts_with("gi|") && !h.starts_with("pdb|") &&
                   !h.starts_with("ref|");
        case header_format::GENBANK:
            return has_bracket_equals(h);
        case header_format::GISAID: {
            if (pipes < 6) return false;
            if (h.empty() || !(h[0] == 'A' || h[0] == 'B')) return false;
            if (h.size() < 2 || h[1] != '/') return false;
            const std::size_t p1 = h.find('|');
            const std::size_t p2 = (p1 == string_view::npos) ? p1 : h.find('|', p1 + 1);
            const std::size_t p3 = (p2 == string_view::npos) ? p2 : h.find('|', p2 + 1);
            if (p3 == string_view::npos) return false;
            const std::size_t p4 = h.find('|', p3 + 1);
            const std::size_t field3_end = (p4 == string_view::npos) ? h.size() : p4;
            return is_date_like(h.substr(p3 + 1, field3_end - (p3 + 1)));
        }
        case header_format::IMGT:
            return pipes == 14;
        default:
            return false;
    }
}

}  // namespace bioseq::io::idparse::detail

// =====================================================================
// §4.5 / §7 — exported entry points
// =====================================================================

export namespace bioseq::io::idparse {

/// Per record: walk `header`, emit values through `routing`, write via `writer`.
[[nodiscard]] std::expected<void, std::string> parse_and_route_header(
        header_format fmt, std::string_view header,
        const identifier_routing& routing, identifier_parse_state& state,
        identifier_writer_ref writer) {
    state.lexer.lex(header);
    detail::route_emitter emit{routing, writer, {}};
    const header_format_def& def = format_def(fmt);
    switch (fmt) {
        case header_format::UNIPROTKB:
            detail::parse_uniprot(header, state, def, emit); break;
        case header_format::UNIREF:
        case header_format::UNIPARC:
        case header_format::UNIMES:
            detail::parse_keyed(header, state, def, emit); break;
        case header_format::SPD:
        case header_format::GISAID:
        case header_format::IMGT:
            detail::parse_positional(header, state, def, emit); break;
        case header_format::NCBI:
        case header_format::NCBI_GI:
        case header_format::NCBI_PDB:
        case header_format::REFSEQ:
            detail::parse_ncbi(header, state, def, emit); break;
        case header_format::GENBANK:
            detail::parse_genbank(header, state, def, emit); break;
        case header_format::PLAIN:
            detail::parse_plain(header, state, def, emit); break;
        case header_format::AUTO:
        default:
            detail::parse_auto(header, state, def, emit); break;
    }
    return {};
}

/// §9.5 — detect the dialect from sampled headers (auto mode), majority vote.
/// Ties break by §4.1 enum order; no match -> PLAIN if no `|`/`=`/`[`, else AUTO.
[[nodiscard]] header_format detect_header_format(
        std::span<const std::string_view> samples) noexcept {
    if (samples.empty()) return header_format::AUTO;

    std::array<int, static_cast<std::size_t>(header_format::COUNT_)> votes{};
    bool any_structure = false;
    for (const std::string_view h : samples) {
        if (h.find('|') != std::string_view::npos ||
            h.find('=') != std::string_view::npos ||
            h.find('[') != std::string_view::npos)
            any_structure = true;
        // Candidate formats only (skip AUTO/PLAIN sentinels at 0/1).
        for (std::size_t f = static_cast<std::size_t>(header_format::UNIPROTKB);
             f < votes.size(); ++f) {
            if (detail::format_matches(static_cast<header_format>(f), h))
                ++votes[f];
        }
    }
    int best = 0;
    header_format best_fmt = header_format::PLAIN;
    for (std::size_t f = static_cast<std::size_t>(header_format::UNIPROTKB);
         f < votes.size(); ++f) {
        if (votes[f] > best) {   // strict '>' keeps the lowest enum on ties
            best = votes[f];
            best_fmt = static_cast<header_format>(f);
        }
    }
    if (best == 0) return any_structure ? header_format::AUTO : header_format::PLAIN;
    return best_fmt;
}

/// §7.1 — resolve the column mapping ONCE per load. Pure logic over plain data.
[[nodiscard]] std::expected<identifier_mapping_result, std::string>
resolve_identifier_mapping(
        header_format requested_fmt, header_format effective_fmt,
        std::span<const column_descriptor> columns,
        std::span<const user_column_mapping> user_maps) {
    identifier_mapping_result result;
    identifier_routing& routing = result.routing;
    std::vector<std::uint16_t>& dest_columns = result.slot_to_col;  // slot -> col

    const std::size_t ncols = columns.size();
    std::vector<char> is_user_mapped(ncols, 0);
    std::vector<char> is_metadata_claimed(ncols, 0);
    std::vector<dest_slot_t> col_to_slot(ncols, -1);
    bool any_keyed_or_pos = false;

    auto slot_of = [&](std::size_t i) -> dest_slot_t {
        if (col_to_slot[i] >= 0) return col_to_slot[i];
        const dest_slot_t s = static_cast<dest_slot_t>(dest_columns.size());
        dest_columns.push_back(static_cast<std::uint16_t>(i));
        col_to_slot[i] = s;
        return s;
    };
    auto contains_slot = [](const std::vector<dest_slot_t>& slots,
                            dest_slot_t s) -> bool {
        return std::find(slots.begin(), slots.end(), s) != slots.end();
    };
    auto add_key_route = [&](std::string key, dest_slot_t s) {
        for (identifier_key_route& route : routing.key_routes) {
            if (route.key == key) {
                if (!contains_slot(route.slots, s)) {
                    route.slots.push_back(s);
                    any_keyed_or_pos = true;
                }
                return;
            }
        }
        routing.key_routes.push_back(identifier_key_route{
            .key = std::move(key),
            .slots = std::vector<dest_slot_t>{s},
        });
        any_keyed_or_pos = true;
    };
    auto add_pos_route = [&](std::uint32_t p, dest_slot_t s) {
        for (identifier_pos_route& route : routing.pos_routes) {
            if (route.position == p) {
                if (!contains_slot(route.slots, s)) {
                    route.slots.push_back(s);
                    any_keyed_or_pos = true;
                }
                return;
            }
        }
        routing.pos_routes.push_back(identifier_pos_route{
            .position = p,
            .slots = std::vector<dest_slot_t>{s},
        });
        any_keyed_or_pos = true;
    };
    auto find_column = [&](std::string_view raw_name) -> std::size_t {
        for (std::size_t i = 0; i < ncols; ++i)
            if (detail::names_match_normalised(columns[i].name, raw_name))
                return i;
        return ncols;
    };

    const header_format_def& eff_def = format_def(effective_fmt);

    // ---- A. explicit user mappings (highest priority) ----
    for (const user_column_mapping& um : user_maps) {
        const std::size_t i = find_column(um.column_name);
        if (i == ncols)
            return std::unexpected(fmt::format(
                "mapping target column \"{}\" not found", um.column_name));
        if (columns[i].is_identity_or_generated)
            return std::unexpected(fmt::format(
                "mapping target column \"{}\" is an identity/generated column "
                "and cannot be populated by bioseq.load", um.column_name));
        is_user_mapped[i] = 1;
        const dest_slot_t slot = slot_of(i);

        const std::string_view ds = detail::trim_spaces(um.data_source);
        std::uint32_t index = 0;
        const auto [ptr, ec] =
            std::from_chars(ds.data(), ds.data() + ds.size(), index);
        const bool is_index = !ds.empty() && ec == std::errc{} &&
                              ptr == ds.data() + ds.size();
        if (is_index) {
            add_pos_route(index, slot);
        } else if (effective_fmt == header_format::AUTO) {
            add_key_route(detail::normalise_to_string(um.data_source), slot);
        } else {
            const header_field_def* fd =
                detail::find_field_for_name(eff_def, um.data_source);
            if (!fd)
                return std::unexpected(fmt::format(
                    "\"{}\" is not a field of format {}",
                    um.data_source, eff_def.sql_name));
            if (fd->origin == field_origin::POSITIONAL)
                add_pos_route(fd->position, slot);
            else
                add_key_route(std::string(fd->norm_key), slot);
        }
    }

    // ---- B. implicit fixed-format field matching for remaining columns ----
    if (effective_fmt != header_format::AUTO &&
        effective_fmt != header_format::PLAIN) {
        for (std::size_t i = 0; i < ncols; ++i) {
            if (is_user_mapped[i]) continue;
            const header_field_def* fd =
                detail::find_field_for_name(eff_def, columns[i].name);
            if (!fd) continue;
            const dest_slot_t s = slot_of(i);
            is_metadata_claimed[i] = 1;
            if (fd->origin == field_origin::POSITIONAL)
                add_pos_route(fd->position, s);
            else
                add_key_route(std::string(fd->norm_key), s);
        }
    }

    // ---- C. choose the identifier column from unclaimed text columns (D7/D8) ----
    boost::container::small_vector<std::size_t, 4> cand, cand_pk, cand_alias;
    for (std::size_t i = 0; i < ncols; ++i) {
        if (!columns[i].is_text || is_user_mapped[i] || is_metadata_claimed[i])
            continue;
        cand.push_back(i);
        if (columns[i].is_primary_key) cand_pk.push_back(i);
        if (detail::names_match_normalised(columns[i].name, "id") ||
            detail::names_match_normalised(columns[i].name, "identifier") ||
            detail::names_match_normalised(columns[i].name, "header") ||
            detail::names_match_normalised(columns[i].name, "name"))
            cand_alias.push_back(i);
    }
    std::size_t identifier_col = ncols;  // "none"
    if (cand_pk.size() == 1)         identifier_col = cand_pk[0];
    else if (cand_alias.size() == 1) identifier_col = cand_alias[0];
    else if (cand.size() == 1)       identifier_col = cand[0];
    else if (cand.size() > 1)
        return std::unexpected(fmt::format(
            "{} unmapped text columns and no single PRIMARY KEY / identifier-name "
            "alias; pass columns => ARRAY[(...)] to disambiguate", cand.size()));
    if (identifier_col != ncols)
        routing.identifier_slots.push_back(slot_of(identifier_col));

    // ---- D. AUTO generic metadata matching for remaining columns ----
    if (requested_fmt == header_format::AUTO) {
        for (std::size_t i = 0; i < ncols; ++i) {
            if (is_user_mapped[i] || i == identifier_col || is_metadata_claimed[i])
                continue;
            add_key_route(detail::normalise_to_string(columns[i].name), slot_of(i));
        }
    }

    // ---- E. strip decision (D7), computed AFTER the dest set is final ----
    routing.strip_metadata =
        (requested_fmt == header_format::AUTO)
            ? any_keyed_or_pos
            : (eff_def.meta != metadata_style::NONE);
    routing.dest_count = dest_columns.size();
    return result;
}

}  // namespace bioseq::io::idparse
