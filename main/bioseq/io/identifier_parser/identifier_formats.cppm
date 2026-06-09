// bioseq/io/identifier_parser/identifier_formats.cppm
//
// DOC0295 §4.1 / §8 — NO_POSTGRES constexpr format + field tables for the FASTA
// /FASTQ header (identifier line) parser. This module carries *no* Postgres
// types: it is pure data describing the known header dialects (UniProtKB, UniRef,
// UniParc, UniMes, SPD, NCBI/RefSeq, GenBank, GISAID, IMGT) plus the
// normalisation rule (§4.2) used to match those field names against table column
// names. The runtime extractors live in `bioseq_io.identifier_parser`.
//
// Source of truth for the tables: the reference parsers in `fasta_parsing_related/`
// (pyteomics_parser.py, pro_fasta.py, uniprot_spec.md) as enumerated in DOC0295 §8.
module;

#include "common.h"            // fmt (DOC0145: common.h + mlogging for fmt::format)

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

export module bioseq_io.identifier_formats;

import mlogging;               // fmt::format
import bioseq_io.file_utils;   // bioseq::io::detail::to_ascii_lower (shared ASCII fold)

static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::io::idparse {

// =====================================================================
// §4.1 — enums
// =====================================================================

/// The header dialect. AUTO is detected once per file from sampled headers.
enum class header_format : std::uint8_t {
    AUTO = 0, PLAIN, UNIPROTKB, UNIREF, UNIPARC, UNIMES, SPD,
    NCBI, NCBI_GI, NCBI_PDB, REFSEQ, GENBANK, GISAID, IMGT,
    COUNT_  // sentinel; never a real format
};

/// Where a field's bytes come from inside a header.
enum class field_origin : std::uint8_t {
    IDENTIFIER,    // the leading id / metadata-stripped header
    POSITIONAL,    // the i-th primary-delimiter ('|' or '/') field
    DESCRIPTION,   // free text between the id block and the first metadata tag
    EQUALS_TAG,    // KEY=VALUE   (value runs to the next recognised tag/end)
    BRACKET_TAG,   // [KEY=VALUE]
};

/// How metadata is encoded in a format.
enum class metadata_style : std::uint8_t { NONE, EQUALS, BRACKET, BOTH };

/// POSITIONAL field marker: "this field is not a positional field".
inline constexpr std::uint16_t kNoPosition = 0xFFFF;

// =====================================================================
// §4.2 — name normalisation (D6): lower-case and drop '_' / '-'
// =====================================================================

/// Lower-case and drop '_' and '-'. Headers/column names are short; scalar.
/// `out` must hold at least `in.size()` bytes; returns the written view into it.
/// Reuses the single ASCII-fold rule in file_utils (`to_ascii_lower`) so the
/// runtime column-name match and the consteval `norm_key` table guards normalise
/// through identical logic — no drift (mirrors pg_compact_sequence.cppm guards).
[[nodiscard]] inline std::string_view normalise_name(
        std::string_view in, std::span<char> out) noexcept {
    std::size_t n = 0;
    for (char c : in) {
        if (c == '_' || c == '-') continue;
        out[n++] = bioseq::io::detail::to_ascii_lower(c);
    }
    return std::string_view{out.data(), n};
}

/// `consteval` companion of `normalise_name`: true iff normalising `raw`
/// (lower-case, drop '_'/'-') yields exactly `normalised`. Used both to validate
/// the hand-written `norm_key` constants at compile time and to compare a runtime
/// name against a constant without a scratch buffer.
[[nodiscard]] constexpr bool normalised_equals(
        std::string_view raw, std::string_view normalised) noexcept {
    std::size_t j = 0;
    for (char c : raw) {
        if (c == '_' || c == '-') continue;
        const char lc = bioseq::io::detail::to_ascii_lower(c);
        if (j >= normalised.size() || normalised[j] != lc) return false;
        ++j;
    }
    return j == normalised.size();
}

// =====================================================================
// §4.1 — field / format table types
// =====================================================================

/// One known field of a format. `norm_key` is the *normalised canonical* key the
/// parser EMITS for this field; `token` is the header short token (e.g. "OS").
/// `aliases` are extra spellings from the reference parsers / common schema
/// styles. At resolution time `norm(canonical)`, `norm(token)`, and `norm(alias)`
/// each match a table column; the extractor always emits the single `norm_key`.
struct header_field_def {
    std::string_view canonical;   // long name, e.g. "organism_name"
    std::string_view token;       // header token, e.g. "OS" ("" if none)
    std::string_view norm_key;    // = norm(canonical); the emitted routing key
    std::span<const std::string_view> aliases;
    field_origin     origin;
    std::uint16_t    position;    // POSITIONAL only; else kNoPosition
};

/// A complete format definition. `fields` spans a per-format static array.
struct header_format_def {
    header_format                     id;
    std::string_view                  sql_name;       // 'uni_prot'
    char                              primary_delim;  // '|', '/', or '\0'
    metadata_style                    meta;
    std::span<const header_field_def> fields;
    std::uint16_t                     id_block_field_count = 0;  // NCBI family (§6.2b)
};

// =====================================================================
// §8 — alias arrays (constexpr static; the field spans point at them)
// =====================================================================

inline constexpr std::array<std::string_view, 2> kAliasesId{{"id", "identifier"}};
inline constexpr std::array<std::string_view, 1> kAliasesEntry{{"entry"}};
inline constexpr std::array<std::string_view, 2> kAliasesProteinName{{"name", "description"}};
inline constexpr std::array<std::string_view, 1> kAliasesDescription{{"description"}};
inline constexpr std::array<std::string_view, 2> kAliasesNcbiId{{"identifier", "accession"}};
inline constexpr std::array<std::string_view, 3> kAliasesGenBankId{{"identifier", "accession", "name"}};
inline constexpr std::array<std::string_view, 2> kAliasesAccessionId{{"id", "identifier"}};
inline constexpr std::array<std::string_view, 1> kAliasesName{{"name"}};

// =====================================================================
// §4.1 / §8 — per-format field tables
// =====================================================================

// --- UNIPROTKB: db|accession|entry block + EQUALS tags (§4.1) ---
inline constexpr std::array<header_field_def, 11> kUniProtKbFields{{
  {"db",                  "",   "db",                 {},                  field_origin::POSITIONAL,  0},
  {"accession",           "",   "accession",          kAliasesId,          field_origin::POSITIONAL,  1},
  {"entry_name",          "",   "entryname",          kAliasesEntry,       field_origin::POSITIONAL,  2},
  {"protein_name",        "",   "proteinname",        kAliasesProteinName, field_origin::DESCRIPTION, kNoPosition},
  {"gene_id",             "",   "geneid",             {},                  field_origin::DESCRIPTION, kNoPosition},
  {"taxon",               "",   "taxon",              {},                  field_origin::DESCRIPTION, kNoPosition},
  {"organism_name",       "OS", "organismname",       {},                  field_origin::EQUALS_TAG,  kNoPosition},
  {"organism_identifier", "OX", "organismidentifier", {},                  field_origin::EQUALS_TAG,  kNoPosition},
  {"gene_name",           "GN", "genename",           {},                  field_origin::EQUALS_TAG,  kNoPosition},
  {"protein_existence",   "PE", "proteinexistence",   {},                  field_origin::EQUALS_TAG,  kNoPosition},
  {"sequence_version",    "SV", "sequenceversion",    {},                  field_origin::EQUALS_TAG,  kNoPosition},
}};

// --- UNIREF: first-token id + cluster + n/Tax/TaxID/RepID (§8) ---
inline constexpr std::array<header_field_def, 6> kUniRefFields{{
  {"id",      "",      "id",     kAliasesId,  field_origin::POSITIONAL,  0},
  {"cluster", "",      "cluster", {},         field_origin::DESCRIPTION, kNoPosition},
  {"n",       "n",     "n",       {},         field_origin::EQUALS_TAG,  kNoPosition},
  {"tax",     "Tax",   "tax",     {},         field_origin::EQUALS_TAG,  kNoPosition},
  {"tax_id",  "TaxID", "taxid",   {},         field_origin::EQUALS_TAG,  kNoPosition},
  {"rep_id",  "RepID", "repid",   {},         field_origin::EQUALS_TAG,  kNoPosition},
}};

// --- UNIPARC simple + proteomes (one format, PC= disambiguates) (§8) ---
inline constexpr std::array<header_field_def, 9> kUniParcFields{{
  {"id",                  "",       "id",                 kAliasesId,          field_origin::POSITIONAL,  0},
  {"protein_name",        "",       "proteinname",        kAliasesProteinName, field_origin::DESCRIPTION, kNoPosition},
  {"status",              "status", "status",             {},                  field_origin::EQUALS_TAG,  kNoPosition},
  {"organism_name",       "OS",     "organismname",       {},                  field_origin::EQUALS_TAG,  kNoPosition},
  {"organism_identifier", "OX",     "organismidentifier", {},                  field_origin::EQUALS_TAG,  kNoPosition},
  {"gene_name",           "GN",     "genename",           {},                  field_origin::EQUALS_TAG,  kNoPosition},
  {"accession_list",      "AC",     "accessionlist",      {},                  field_origin::EQUALS_TAG,  kNoPosition},
  {"source_id_list",      "SS",     "sourceidlist",       {},                  field_origin::EQUALS_TAG,  kNoPosition},
  {"proteome_component",  "PC",     "proteomecomponent",  {},                  field_origin::EQUALS_TAG,  kNoPosition},
}};

// --- UNIMES: first-token id + name + OS/SV + arbitrary tags (§8) ---
inline constexpr std::array<header_field_def, 4> kUniMesFields{{
  {"id",               "",   "id",              kAliasesId,          field_origin::POSITIONAL,  0},
  {"name",             "",   "name",            kAliasesDescription, field_origin::DESCRIPTION, kNoPosition},
  {"organism_name",    "OS", "organismname",    {},                  field_origin::EQUALS_TAG,  kNoPosition},
  {"sequence_version", "SV", "sequenceversion", {},                  field_origin::EQUALS_TAG,  kNoPosition},
}};

// --- SPD: id|gene|description, gene split on '_' -> gene_id/taxon (§8) ---
inline constexpr std::array<header_field_def, 5> kSpdFields{{
  {"id",          "", "id",          kAliasesId, field_origin::POSITIONAL,  0},
  {"gene",        "", "gene",        {},         field_origin::POSITIONAL,  1},
  {"description", "", "description", {},         field_origin::POSITIONAL,  2},
  {"gene_id",     "", "geneid",      {},         field_origin::DESCRIPTION, kNoPosition},
  {"taxon",       "", "taxon",       {},         field_origin::DESCRIPTION, kNoPosition},
}};

// --- NCBI generic: id + description + trailing [organism] (§8) ---
inline constexpr std::array<header_field_def, 4> kNcbiFields{{
  {"id",          "", "id",          kAliasesNcbiId, field_origin::POSITIONAL,  0},
  {"description", "", "description", {},             field_origin::DESCRIPTION, kNoPosition},
  {"organism",    "", "organism",    {},             field_origin::BRACKET_TAG, kNoPosition},
  {"taxon",       "", "taxon",       {},             field_origin::BRACKET_TAG, kNoPosition},
}};

// --- NCBI gi: gi|gi_number|... + trailing [organism] (§8) ---
inline constexpr std::array<header_field_def, 5> kNcbiGiFields{{
  {"gi",          "", "gi",          {},         field_origin::POSITIONAL,  0},
  {"gi_number",   "", "ginumber",    kAliasesId, field_origin::POSITIONAL,  1},
  {"description", "", "description", {},         field_origin::DESCRIPTION, kNoPosition},
  {"organism",    "", "organism",    {},         field_origin::BRACKET_TAG, kNoPosition},
  {"taxon",       "", "taxon",       {},         field_origin::BRACKET_TAG, kNoPosition},
}};

// --- NCBI pdb: pdb|pdb_id|chain description + trailing [organism] (§8) ---
// `chain` is DESCRIPTION-origin (key route) because parse_ncbi splits it out of
// parts[2] (MEDIUM-04); it must not bind the whole parts[2] via a pos route.
inline constexpr std::array<header_field_def, 6> kNcbiPdbFields{{
  {"pdb",         "", "pdb",         {},                 field_origin::POSITIONAL,  0},
  {"pdb_id",      "", "pdbid",       kAliasesAccessionId, field_origin::POSITIONAL,  1},
  {"chain",       "", "chain",       {},                 field_origin::DESCRIPTION, kNoPosition},
  {"description", "", "description", {},                 field_origin::DESCRIPTION, kNoPosition},
  {"organism",    "", "organism",    {},                 field_origin::BRACKET_TAG, kNoPosition},
  {"taxon",       "", "taxon",       {},                 field_origin::BRACKET_TAG, kNoPosition},
}};

// --- RefSeq: ref|accession| description [organism] (§8) ---
inline constexpr std::array<header_field_def, 5> kRefSeqFields{{
  {"ref",         "", "ref",         {},                  field_origin::POSITIONAL,  0},
  {"accession",   "", "accession",   kAliasesAccessionId, field_origin::POSITIONAL,  1},
  {"description", "", "description", {},                  field_origin::DESCRIPTION, kNoPosition},
  {"organism",    "", "organism",    {},                  field_origin::BRACKET_TAG, kNoPosition},
  {"taxon",       "", "taxon",       {},                  field_origin::BRACKET_TAG, kNoPosition},
}};

// --- GenBank: id + [key=value] bracket tags (§8) ---
inline constexpr std::array<header_field_def, 5> kGenBankFields{{
  {"id",         "", "id",         kAliasesGenBankId, field_origin::POSITIONAL,  0},
  {"organism",   "", "organism",   {},                field_origin::BRACKET_TAG, kNoPosition},
  {"chromosome", "", "chromosome", {},                field_origin::BRACKET_TAG, kNoPosition},
  {"map",        "", "map",        {},                field_origin::BRACKET_TAG, kNoPosition},
  {"moltype",    "", "moltype",    {},                field_origin::BRACKET_TAG, kNoPosition},
}};

// --- GISAID: 8 pipe-positional fields (§8) ---
inline constexpr std::array<header_field_def, 8> kGisaidFields{{
  {"strain",          "", "strain",         kAliasesName,        field_origin::POSITIONAL, 0},
  {"type",            "", "type",           {},                  field_origin::POSITIONAL, 1},
  {"subtype",         "", "subtype",        {},                  field_origin::POSITIONAL, 2},
  {"collection_date", "", "collectiondate", kAliasesDescription, field_origin::POSITIONAL, 3},
  {"passage",         "", "passage",        {},                  field_origin::POSITIONAL, 4},
  {"country",         "", "country",        {},                  field_origin::POSITIONAL, 5},
  {"host",            "", "host",           {},                  field_origin::POSITIONAL, 6},
  {"accession",       "", "accession",      kAliasesId,          field_origin::POSITIONAL, 7},
}};

// --- IMGT: 15 pipe-positional fields (§8) ---
inline constexpr std::array<header_field_def, 15> kImgtFields{{
  {"ligm_accession",      "", "ligmaccession",     kAliasesAccessionId, field_origin::POSITIONAL, 0},
  {"imgt_allele_name",    "", "imgtallelename",    {},                  field_origin::POSITIONAL, 1},
  {"species",             "", "species",           {},                  field_origin::POSITIONAL, 2},
  {"functionality",       "", "functionality",     {},                  field_origin::POSITIONAL, 3},
  {"region",              "", "region",            {},                  field_origin::POSITIONAL, 4},
  {"positions",           "", "positions",         {},                  field_origin::POSITIONAL, 5},
  {"nt_count",            "", "ntcount",           {},                  field_origin::POSITIONAL, 6},
  {"codon_start",         "", "codonstart",        {},                  field_origin::POSITIONAL, 7},
  {"nt_added_5p",         "", "ntadded5p",         {},                  field_origin::POSITIONAL, 8},
  {"nt_added_removed_3p", "", "ntaddedremoved3p",  {},                  field_origin::POSITIONAL, 9},
  {"nt_corrections",      "", "ntcorrections",     {},                  field_origin::POSITIONAL, 10},
  {"aa_count",            "", "aacount",           {},                  field_origin::POSITIONAL, 11},
  {"seq_length",          "", "seqlength",         {},                  field_origin::POSITIONAL, 12},
  {"partial",             "", "partial",           {},                  field_origin::POSITIONAL, 13},
  {"reverse_complement",  "", "reversecomplement", {},                  field_origin::POSITIONAL, 14},
}};

inline constexpr std::array<header_field_def, 0> kNoFields{};

// =====================================================================
// §4.1 — format definition table, indexed by enum value
// =====================================================================

inline constexpr std::array<header_format_def,
        static_cast<std::size_t>(header_format::COUNT_)> kFormatDefs{{
  // id                       sql_name      delim  meta                          fields           idblock
  {header_format::AUTO,      "auto",     '\0', metadata_style::BOTH,    kNoFields,       0},
  {header_format::PLAIN,     "plain",    '\0', metadata_style::NONE,    kNoFields,       0},
  {header_format::UNIPROTKB, "uni_prot", '|',  metadata_style::EQUALS,  kUniProtKbFields, 0},
  {header_format::UNIREF,    "uni_ref",  '\0', metadata_style::EQUALS,  kUniRefFields,   0},
  {header_format::UNIPARC,   "uni_parc", '\0', metadata_style::EQUALS,  kUniParcFields,  0},
  {header_format::UNIMES,    "uni_mes",  '\0', metadata_style::EQUALS,  kUniMesFields,   0},
  {header_format::SPD,       "spd",      '|',  metadata_style::NONE,    kSpdFields,      0},
  {header_format::NCBI,      "ncbi",     '\0', metadata_style::BRACKET, kNcbiFields,     0},
  {header_format::NCBI_GI,   "ncbi_gi",  '|',  metadata_style::BRACKET, kNcbiGiFields,   2},
  {header_format::NCBI_PDB,  "ncbi_pdb", '|',  metadata_style::BRACKET, kNcbiPdbFields,  2},
  {header_format::REFSEQ,    "refseq",   '|',  metadata_style::BRACKET, kRefSeqFields,   2},
  {header_format::GENBANK,   "genbank",  '\0', metadata_style::BRACKET, kGenBankFields,  0},
  {header_format::GISAID,    "gisaid",   '|',  metadata_style::NONE,    kGisaidFields,   0},
  {header_format::IMGT,      "imgt",     '|',  metadata_style::NONE,    kImgtFields,     0},
}};

/// O(1) lookup: header_format -> its definition.
[[nodiscard]] inline const header_format_def& format_def(header_format f) noexcept {
    return kFormatDefs[static_cast<std::size_t>(f)];
}

// =====================================================================
// Compile-time drift guards: every field's norm_key == norm(canonical),
// and the format table is dense + ordered by enum value (mirrors the
// pg_compact_sequence.cppm:82-100 source-of-truth static_asserts).
// =====================================================================

[[nodiscard]] consteval bool all_norm_keys_canonical() {
    for (const auto& def : kFormatDefs)
        for (const auto& fd : def.fields)
            if (!normalised_equals(fd.canonical, fd.norm_key))
                return false;
    return true;
}
static_assert(all_norm_keys_canonical(),
              "header_field_def.norm_key drifted from norm(canonical) — DOC0295 §4.1");

[[nodiscard]] consteval bool format_table_is_dense_and_ordered() {
    for (std::size_t i = 0; i < kFormatDefs.size(); ++i)
        if (static_cast<std::size_t>(kFormatDefs[i].id) != i) return false;
    return true;
}
static_assert(format_table_is_dense_and_ordered(),
              "kFormatDefs must be dense and ordered by header_format value");

// =====================================================================
// §4.1 — SQL format-name parsing (normalisation-insensitive)
// =====================================================================

/// Map a SQL format name ('auto','uni_prot','uni_ref',…) to its enum,
/// case/separator-insensitively. Returns a human-readable error listing the
/// valid names on no match (mapped to ereport by the PG caller).
[[nodiscard]] inline std::expected<header_format, std::string>
header_format_from_sql_name(std::string_view name) {
    std::array<char, 64> scratch{};
    if (name.size() > scratch.size())
        return std::unexpected(fmt::format(
            "unknown header_format \"{}\"", name));
    const std::string_view normalised = normalise_name(name, scratch);
    for (const auto& def : kFormatDefs) {
        if (normalised_equals(def.sql_name, normalised))
            return def.id;
    }
    std::string valid;
    for (const auto& def : kFormatDefs) {
        if (!valid.empty()) valid += ", ";
        valid += '\'';
        valid += def.sql_name;
        valid += '\'';
    }
    return std::unexpected(fmt::format(
        "unknown header_format \"{}\"; valid values are {}", name, valid));
}

}  // namespace bioseq::io::idparse
