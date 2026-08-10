/**
 * @file formats.cppm
 * @brief PostgreSQL-independent sequence-header dialect definitions.
 *
 * The constexpr tables describe known FASTA and FASTQ header dialects and the
 * normalisation rule used to match their fields to destination columns. Runtime
 * extraction lives in `bioseq.io.identifier_parser`.
 */
module;


#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

export module bioseq.io.identifier_parser.formats;

import bioseq.common.logging;               // fmt::format
import bioseq.io.file_kind;   // bioseq::io::detail::lowercase_ascii_character (shared ASCII fold)

static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::io::identifier_parser {

// =====================================================================
// §4.1 — enums
// =====================================================================

/// The header dialect. AUTO is detected once per file from sampled headers.
enum class header_format : std::uint8_t {
    automatic = 0, plain, uniprotkb, uniref, uniparc, unimes, spd,
    ncbi, ncbi_gi, ncbi_pdb, refseq, genbank, gisaid, imgt,
    count  // sentinel; never a real format
};

/// Where a field's bytes come from inside a header.
enum class field_origin : std::uint8_t {
    identifier,    // the leading id / metadata-stripped header
    positional,    // the i-th primary-delimiter ('|' or '/') field
    description,   // free text between the id block and the first metadata tag
    equals_tag,    // KEY=VALUE   (value runs to the next recognised tag/end)
    bracket_tag,   // [KEY=VALUE]
};

/// How metadata is encoded in a format.
enum class metadata_style : std::uint8_t { none, equals, bracket, both };

/// positional field marker: "this field is not a positional field".
inline constexpr std::uint16_t no_position = 0xFFFF;

// =====================================================================
// §4.2 — name normalisation (D6): lower-case and drop '_' / '-'
// =====================================================================

/// Lower-case and drop '_' and '-'. Headers/column names are short; scalar.
/// `out` must hold at least `in.size()` bytes; returns the written view into it.
/// Reuses the single ASCII-fold rule in file_utils (`lowercase_ascii_character`) so the
/// runtime column-name match and the consteval `normalised_key` table guards normalise
/// through identical logic — no drift (mirrors pg_compact_sequence.cppm guards).
[[nodiscard]] inline std::string_view normalise_name(
        std::string_view in, std::span<char> out) noexcept {
    std::size_t n = 0;
    for (char c : in) {
        if (c == '_' || c == '-') continue;
        out[n++] = bioseq::io::detail::lowercase_ascii_character(c);
    }
    return std::string_view{out.data(), n};
}

/// `consteval` companion of `normalise_name`: true iff normalising `raw`
/// (lower-case, drop '_'/'-') yields exactly `normalised`. Used both to validate
/// the hand-written `normalised_key` constants at compile time and to compare a runtime
/// name against a constant without a scratch buffer.
[[nodiscard]] constexpr bool normalised_equals(
        std::string_view raw, std::string_view normalised) noexcept {
    std::size_t j = 0;
    for (char c : raw) {
        if (c == '_' || c == '-') continue;
        const char lc = bioseq::io::detail::lowercase_ascii_character(c);
        if (j >= normalised.size() || normalised[j] != lc) return false;
        ++j;
    }
    return j == normalised.size();
}

// =====================================================================
// §4.1 — field / format table types
// =====================================================================

/// One known field of a format. `normalised_key` is the *normalised canonical* key the
/// parser EMITS for this field; `token` is the header short token (e.g. "OS").
/// `aliases` are extra spellings from the reference parsers / common schema
/// styles. At resolution time `norm(canonical)`, `norm(token)`, and `norm(alias)`
/// each match a table column; the extractor always emits the single `normalised_key`.
struct header_field_definition {
    std::string_view canonical;   // long name, e.g. "organism_name"
    std::string_view token;       // header token, e.g. "OS" ("" if none)
    std::string_view normalised_key;    // = norm(canonical); the emitted routing key
    std::span<const std::string_view> aliases;
    field_origin     origin;
    std::uint16_t    position;    // positional only; else no_position
};

/// A complete format definition. `fields` spans a per-format static array.
struct header_format_definition {
    header_format                     id;
    std::string_view                  sql_name;       // 'uni_prot'
    char                              primary_delimiter;  // '|', '/', or '\0'
    metadata_style                    metadata;
    std::span<const header_field_definition> fields;
    std::uint16_t                     identifier_block_field_count = 0;  // ncbi family (§6.2b)
};

// =====================================================================
// §8 — alias arrays (constexpr static; the field spans point at them)
// =====================================================================

inline constexpr std::array<std::string_view, 2> identifier_aliases{{"id", "identifier"}};
inline constexpr std::array<std::string_view, 1> entry_aliases{{"entry"}};
inline constexpr std::array<std::string_view, 2> protein_name_aliases{{"name", "description"}};
inline constexpr std::array<std::string_view, 1> description_aliases{{"description"}};
inline constexpr std::array<std::string_view, 2> ncbi_identifier_aliases{{"identifier", "accession"}};
inline constexpr std::array<std::string_view, 3> genbank_identifier_aliases{{"identifier", "accession", "name"}};
inline constexpr std::array<std::string_view, 2> accession_identifier_aliases{{"id", "identifier"}};
inline constexpr std::array<std::string_view, 1> name_aliases{{"name"}};

// =====================================================================
// §4.1 / §8 — per-format field tables
// =====================================================================

// --- uniprotkb: db|accession|entry block + EQUALS tags (§4.1) ---
inline constexpr std::array<header_field_definition, 11> uniprotkb_fields{{
  {"db",                  "",   "db",                 {},                  field_origin::positional,  0},
  {"accession",           "",   "accession",          identifier_aliases,          field_origin::positional,  1},
  {"entry_name",          "",   "entryname",          entry_aliases,       field_origin::positional,  2},
  {"protein_name",        "",   "proteinname",        protein_name_aliases, field_origin::description, no_position},
  {"gene_id",             "",   "geneid",             {},                  field_origin::description, no_position},
  {"taxon",               "",   "taxon",              {},                  field_origin::description, no_position},
  {"organism_name",       "OS", "organismname",       {},                  field_origin::equals_tag,  no_position},
  {"organism_identifier", "OX", "organismidentifier", {},                  field_origin::equals_tag,  no_position},
  {"gene_name",           "GN", "genename",           {},                  field_origin::equals_tag,  no_position},
  {"protein_existence",   "PE", "proteinexistence",   {},                  field_origin::equals_tag,  no_position},
  {"sequence_version",    "SV", "sequenceversion",    {},                  field_origin::equals_tag,  no_position},
}};

// --- uniref: first-token id + cluster + n/Tax/TaxID/RepID (§8) ---
inline constexpr std::array<header_field_definition, 6> uniref_fields{{
  {"id",      "",      "id",     identifier_aliases,  field_origin::positional,  0},
  {"cluster", "",      "cluster", {},         field_origin::description, no_position},
  {"n",       "n",     "n",       {},         field_origin::equals_tag,  no_position},
  {"tax",     "Tax",   "tax",     {},         field_origin::equals_tag,  no_position},
  {"tax_id",  "TaxID", "taxid",   {},         field_origin::equals_tag,  no_position},
  {"rep_id",  "RepID", "repid",   {},         field_origin::equals_tag,  no_position},
}};

// --- uniparc simple + proteomes (one format, PC= disambiguates) (§8) ---
inline constexpr std::array<header_field_definition, 9> uniparc_fields{{
  {"id",                  "",       "id",                 identifier_aliases,          field_origin::positional,  0},
  {"protein_name",        "",       "proteinname",        protein_name_aliases, field_origin::description, no_position},
  {"status",              "status", "status",             {},                  field_origin::equals_tag,  no_position},
  {"organism_name",       "OS",     "organismname",       {},                  field_origin::equals_tag,  no_position},
  {"organism_identifier", "OX",     "organismidentifier", {},                  field_origin::equals_tag,  no_position},
  {"gene_name",           "GN",     "genename",           {},                  field_origin::equals_tag,  no_position},
  {"accession_list",      "AC",     "accessionlist",      {},                  field_origin::equals_tag,  no_position},
  {"source_id_list",      "SS",     "sourceidlist",       {},                  field_origin::equals_tag,  no_position},
  {"proteome_component",  "PC",     "proteomecomponent",  {},                  field_origin::equals_tag,  no_position},
}};

// --- unimes: first-token id + name + OS/SV + arbitrary tags (§8) ---
inline constexpr std::array<header_field_definition, 4> unimes_fields{{
  {"id",               "",   "id",              identifier_aliases,          field_origin::positional,  0},
  {"name",             "",   "name",            description_aliases, field_origin::description, no_position},
  {"organism_name",    "OS", "organismname",    {},                  field_origin::equals_tag,  no_position},
  {"sequence_version", "SV", "sequenceversion", {},                  field_origin::equals_tag,  no_position},
}};

// --- spd: id|gene|description, gene split on '_' -> gene_id/taxon (§8) ---
inline constexpr std::array<header_field_definition, 5> spd_fields{{
  {"id",          "", "id",          identifier_aliases, field_origin::positional,  0},
  {"gene",        "", "gene",        {},         field_origin::positional,  1},
  {"description", "", "description", {},         field_origin::positional,  2},
  {"gene_id",     "", "geneid",      {},         field_origin::description, no_position},
  {"taxon",       "", "taxon",       {},         field_origin::description, no_position},
}};

// --- ncbi generic: id + description + trailing [organism] (§8) ---
inline constexpr std::array<header_field_definition, 4> ncbi_fields{{
  {"id",          "", "id",          ncbi_identifier_aliases, field_origin::positional,  0},
  {"description", "", "description", {},             field_origin::description, no_position},
  {"organism",    "", "organism",    {},             field_origin::bracket_tag, no_position},
  {"taxon",       "", "taxon",       {},             field_origin::bracket_tag, no_position},
}};

// --- ncbi gi: gi|gi_number|... + trailing [organism] (§8) ---
inline constexpr std::array<header_field_definition, 5> ncbi_gi_fields{{
  {"gi",          "", "gi",          {},         field_origin::positional,  0},
  {"gi_number",   "", "ginumber",    identifier_aliases, field_origin::positional,  1},
  {"description", "", "description", {},         field_origin::description, no_position},
  {"organism",    "", "organism",    {},         field_origin::bracket_tag, no_position},
  {"taxon",       "", "taxon",       {},         field_origin::bracket_tag, no_position},
}};

// --- ncbi pdb: pdb|pdb_id|chain description + trailing [organism] (§8) ---
// `chain` is description-origin (key route) because parse_ncbi splits it out of
// parts[2] (MEDIUM-04); it must not bind the whole parts[2] via a pos route.
inline constexpr std::array<header_field_definition, 6> ncbi_pdb_fields{{
  {"pdb",         "", "pdb",         {},                 field_origin::positional,  0},
  {"pdb_id",      "", "pdbid",       accession_identifier_aliases, field_origin::positional,  1},
  {"chain",       "", "chain",       {},                 field_origin::description, no_position},
  {"description", "", "description", {},                 field_origin::description, no_position},
  {"organism",    "", "organism",    {},                 field_origin::bracket_tag, no_position},
  {"taxon",       "", "taxon",       {},                 field_origin::bracket_tag, no_position},
}};

// --- RefSeq: ref|accession| description [organism] (§8) ---
inline constexpr std::array<header_field_definition, 5> refseq_fields{{
  {"ref",         "", "ref",         {},                  field_origin::positional,  0},
  {"accession",   "", "accession",   accession_identifier_aliases, field_origin::positional,  1},
  {"description", "", "description", {},                  field_origin::description, no_position},
  {"organism",    "", "organism",    {},                  field_origin::bracket_tag, no_position},
  {"taxon",       "", "taxon",       {},                  field_origin::bracket_tag, no_position},
}};

// --- GenBank: id + [key=value] bracket tags (§8) ---
inline constexpr std::array<header_field_definition, 5> genbank_fields{{
  {"id",         "", "id",         genbank_identifier_aliases, field_origin::positional,  0},
  {"organism",   "", "organism",   {},                field_origin::bracket_tag, no_position},
  {"chromosome", "", "chromosome", {},                field_origin::bracket_tag, no_position},
  {"map",        "", "map",        {},                field_origin::bracket_tag, no_position},
  {"moltype",    "", "moltype",    {},                field_origin::bracket_tag, no_position},
}};

// --- gisaid: 8 pipe-positional fields (§8) ---
inline constexpr std::array<header_field_definition, 8> gisaid_fields{{
  {"strain",          "", "strain",         name_aliases,        field_origin::positional, 0},
  {"type",            "", "type",           {},                  field_origin::positional, 1},
  {"subtype",         "", "subtype",        {},                  field_origin::positional, 2},
  {"collection_date", "", "collectiondate", description_aliases, field_origin::positional, 3},
  {"passage",         "", "passage",        {},                  field_origin::positional, 4},
  {"country",         "", "country",        {},                  field_origin::positional, 5},
  {"host",            "", "host",           {},                  field_origin::positional, 6},
  {"accession",       "", "accession",      identifier_aliases,          field_origin::positional, 7},
}};

// --- imgt: 15 pipe-positional fields (§8) ---
inline constexpr std::array<header_field_definition, 15> imgt_fields{{
  {"ligm_accession",      "", "ligmaccession",     accession_identifier_aliases, field_origin::positional, 0},
  {"imgt_allele_name",    "", "imgtallelename",    {},                  field_origin::positional, 1},
  {"species",             "", "species",           {},                  field_origin::positional, 2},
  {"functionality",       "", "functionality",     {},                  field_origin::positional, 3},
  {"region",              "", "region",            {},                  field_origin::positional, 4},
  {"positions",           "", "positions",         {},                  field_origin::positional, 5},
  {"nt_count",            "", "ntcount",           {},                  field_origin::positional, 6},
  {"codon_start",         "", "codonstart",        {},                  field_origin::positional, 7},
  {"nt_added_5p",         "", "ntadded5p",         {},                  field_origin::positional, 8},
  {"nt_added_removed_3p", "", "ntaddedremoved3p",  {},                  field_origin::positional, 9},
  {"nt_corrections",      "", "ntcorrections",     {},                  field_origin::positional, 10},
  {"aa_count",            "", "aacount",           {},                  field_origin::positional, 11},
  {"seq_length",          "", "seqlength",         {},                  field_origin::positional, 12},
  {"partial",             "", "partial",           {},                  field_origin::positional, 13},
  {"reverse_complement",  "", "reversecomplement", {},                  field_origin::positional, 14},
}};

inline constexpr std::array<header_field_definition, 0> no_fields{};

// =====================================================================
// §4.1 — format definition table, indexed by enum value
// =====================================================================

inline constexpr std::array<header_format_definition,
        static_cast<std::size_t>(header_format::count)> header_format_definitions{{
  // id                       sql_name      delim  metadata                          fields           idblock
  {header_format::automatic,      "auto",     '\0', metadata_style::both,    no_fields,       0},
  {header_format::plain,     "plain",    '\0', metadata_style::none,    no_fields,       0},
  {header_format::uniprotkb, "uni_prot", '|',  metadata_style::equals,  uniprotkb_fields, 0},
  {header_format::uniref,    "uni_ref",  '\0', metadata_style::equals,  uniref_fields,   0},
  {header_format::uniparc,   "uni_parc", '\0', metadata_style::equals,  uniparc_fields,  0},
  {header_format::unimes,    "uni_mes",  '\0', metadata_style::equals,  unimes_fields,   0},
  {header_format::spd,       "spd",      '|',  metadata_style::none,    spd_fields,      0},
  {header_format::ncbi,      "ncbi",     '\0', metadata_style::bracket, ncbi_fields,     0},
  {header_format::ncbi_gi,   "ncbi_gi",  '|',  metadata_style::bracket, ncbi_gi_fields,   2},
  {header_format::ncbi_pdb,  "ncbi_pdb", '|',  metadata_style::bracket, ncbi_pdb_fields,  2},
  {header_format::refseq,    "refseq",   '|',  metadata_style::bracket, refseq_fields,   2},
  {header_format::genbank,   "genbank",  '\0', metadata_style::bracket, genbank_fields,  0},
  {header_format::gisaid,    "gisaid",   '|',  metadata_style::none,    gisaid_fields,   0},
  {header_format::imgt,      "imgt",     '|',  metadata_style::none,    imgt_fields,     0},
}};

/// O(1) lookup: header_format -> its definition.
[[nodiscard]] inline const header_format_definition& header_format_definition_for(header_format f) noexcept {
    return header_format_definitions[static_cast<std::size_t>(f)];
}

// =====================================================================
// Compile-time drift guards: every field's normalised_key == norm(canonical),
// and the format table is dense + ordered by enum value (mirrors the
// pg_compact_sequence.cppm:82-100 source-of-truth static_asserts).
// =====================================================================

[[nodiscard]] consteval bool all_norm_keys_canonical() {
    for (const auto& def : header_format_definitions)
        for (const auto& fd : def.fields)
            if (!normalised_equals(fd.canonical, fd.normalised_key))
                return false;
    return true;
}
static_assert(all_norm_keys_canonical(),
              "header field keys must match normalised canonical names");

[[nodiscard]] consteval bool format_table_is_dense_and_ordered() {
    for (std::size_t i = 0; i < header_format_definitions.size(); ++i)
        if (static_cast<std::size_t>(header_format_definitions[i].id) != i) return false;
    return true;
}
static_assert(format_table_is_dense_and_ordered(),
              "header_format_definitions must be dense and ordered by header_format value");

// =====================================================================
// §4.1 — SQL format-name parsing (normalisation-insensitive)
// =====================================================================

/// Map a SQL format name ('auto','uni_prot','uni_ref',…) to its enum,
/// case/separator-insensitively. Returns a human-readable error listing the
/// valid names on no match (mapped to ereport by the PG caller).
[[nodiscard]] inline std::expected<header_format, std::string>
parse_header_format_name(std::string_view name) {
    std::array<char, 64> scratch{};
    if (name.size() > scratch.size())
        return std::unexpected(fmt::format(
            "unknown header_format \"{}\"", name));
    const std::string_view normalised = normalise_name(name, scratch);
    for (const auto& def : header_format_definitions) {
        if (normalised_equals(def.sql_name, normalised))
            return def.id;
    }
    std::string valid;
    for (const auto& def : header_format_definitions) {
        if (!valid.empty()) valid += ", ";
        valid += '\'';
        valid += def.sql_name;
        valid += '\'';
    }
    return std::unexpected(fmt::format(
        "unknown header_format \"{}\"; valid values are {}", name, valid));
}

}  // namespace bioseq::io::identifier_parser
