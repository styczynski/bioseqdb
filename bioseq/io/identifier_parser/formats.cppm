/**
 * @file formats.cppm
 * @brief Known FASTA and FASTQ header formats and their fields.
 *
 * These tables map header fields to column names. The identifier parser
 * uses them to read values from each header.
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

import bioseq.common.logging; // fmt::format
import bioseq.io.file_kind;   // bioseq::io::detail::lowercase_ascii_character (shared ASCII fold)

static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::io::identifier_parser {

// Header format types.

/// Header  format. With `automatic`, the loader can sample headers to choose
/// a format. Unrecognized structured headers use generic tags.
enum class header_format : std::uint8_t {
    automatic = 0,
    plain,
    uniprotkb,
    uniref,
    uniparc,
    unimes,
    spd,
    ncbi,
    ncbi_gi,
    ncbi_pdb,
    refseq,
    genbank,
    gisaid,
    imgt,
    count // Sentinel value that does not represent a real format.
};

/// Canonical native SQL enum labels, shared by parsing and SQL generation.
inline constexpr std::array<std::string_view, 14> header_format_sql_names{
    "auto", "plain", "uniprot", "uniref", "uniparc", "unimes", "spd",
    "ncbi", "ncbi_gi", "ncbi_pdb", "refseq", "genbank", "gisaid", "imgt"
};
static_assert(header_format_sql_names.size()
    == static_cast<std::size_t>(header_format::count));

/// Where a field's bytes come from inside a header.
enum class field_origin : std::uint8_t {
    identifier,  // whole header, with optional metadata removal
    positional,  // zero-based field separated by the format delimiter
    description, // free text between the id block and the first metadata tag
    equals_tag,  // KEY=VALUE   (value runs to the next recognised tag/end)
    bracket_tag, // [KEY=VALUE]
};

/// How metadata is encoded in a format.
enum class metadata_style : std::uint8_t { none, equals, bracket, both };

/// Marks a field that has no positional index.
inline constexpr std::uint16_t no_position = 0xFFFF;

// Name normalization.

/// Normalizes names by lowercasing ASCII letters and removing '_' and '-'.
/// Writes into `out`, which must hold `in.size()` bytes.
/// The returned view borrows that buffer.
[[nodiscard]] inline std::string_view normalise_name(std::string_view in,
                                                     std::span<char> out) noexcept {
    std::size_t written_count = 0;
    for (char c : in) {
        if (c == '_' || c == '-')
            continue;
        out[written_count++] = bioseq::io::detail::lowercase_ascii_character(c);
    }
    return std::string_view{out.data(), written_count};
}

/// Compares a raw, name with an already normalized name without a scratch  buffer.
/// Used for compile-timy table checks and runtime format lookup.
[[nodiscard]] constexpr bool normalised_equals(std::string_view raw,
                                               std::string_view normalised) noexcept {
    std::size_t j = 0;
    for (char c : raw) {
        if (c == '_' || c == '-')
            continue;
        const char lc = bioseq::io::detail::lowercase_ascii_character(c);
        if (j >= normalised.size() || normalised[j] != lc)
            return false;
        ++j;
    }
    return j == normalised.size();
}

// Field and format definitions.

/// One header field and the names a destination column can use to select it
/// A column may match the full name, a short token such as "OS", or an alias.
/// Named routes use `normalised_key` when the parser sends the field to a writer.
struct header_field_definition {
    std::string_view canonical;      // long name, e.g. "organism_name"
    std::string_view token;          // header token, e.g. "OS" ("" if none)
    std::string_view normalised_key; // Normalized canonical key emitted for routing.
    std::span<const std::string_view> aliases;
    field_origin origin;
    std::uint16_t position; // Positional index, or no_position for other origins.
};

/// Delimiter, metadata rules, and fields for one header format.
/// The field span refers to a static table.
struct header_format_definition {
    header_format id;
    std::string_view sql_name; // Canonical SQL format name, for example 'uniprot'.
    char primary_delimiter;    // field delimiter, or '\0' when fields are  not delimited
    metadata_style metadata;
    std::span<const header_field_definition> fields;
    std::uint16_t identifier_block_field_count = 0; // number of prefix fields before the NCBI description or PDB chain
    /// Synthetic recipe: {0} is a safe generated component, {1} its numeric identity.
    std::string_view synthetic_pattern;
};

// Alternate field names.

inline constexpr std::array<std::string_view, 2> identifier_aliases{{"id", "identifier"}};
inline constexpr std::array<std::string_view, 1> entry_aliases{{"entry"}};
inline constexpr std::array<std::string_view, 2> protein_name_aliases{{"name", "description"}};
inline constexpr std::array<std::string_view, 1> description_aliases{{"description"}};
inline constexpr std::array<std::string_view, 2> ncbi_identifier_aliases{
    {"identifier", "accession"}};
inline constexpr std::array<std::string_view, 3> genbank_identifier_aliases{
    {"identifier", "accession", "name"}};
inline constexpr std::array<std::string_view, 2> accession_identifier_aliases{{"id", "identifier"}};
inline constexpr std::array<std::string_view, 1> name_aliases{{"name"}};

// Fields for each supported format.

// UniProtKB: db|accession|entry block plus equals tags.
inline constexpr std::array<header_field_definition, 11> uniprotkb_fields{{
    {"db", "", "db", {}, field_origin::positional, 0},
    {"accession", "", "accession", identifier_aliases, field_origin::positional, 1},
    {"entry_name", "", "entryname", entry_aliases, field_origin::positional, 2},
    {"protein_name",
     "",
     "proteinname",
     protein_name_aliases,
     field_origin::description,
     no_position},
    {"gene_id", "", "geneid", {}, field_origin::description, no_position},
    {"taxon", "", "taxon", {}, field_origin::description, no_position},
    {"organism_name", "OS", "organismname", {}, field_origin::equals_tag, no_position},
    {"organism_identifier", "OX", "organismidentifier", {}, field_origin::equals_tag, no_position},
    {"gene_name", "GN", "genename", {}, field_origin::equals_tag, no_position},
    {"protein_existence", "PE", "proteinexistence", {}, field_origin::equals_tag, no_position},
    {"sequence_version", "SV", "sequenceversion", {}, field_origin::equals_tag, no_position},
}};

// UniRef: first-token id plus cluster and keyed metadata.
inline constexpr std::array<header_field_definition, 6> uniref_fields{{
    {"id", "", "id", identifier_aliases, field_origin::positional, 0},
    {"cluster", "", "cluster", {}, field_origin::description, no_position},
    {"n", "n", "n", {}, field_origin::equals_tag, no_position},
    {"tax", "Tax", "tax", {}, field_origin::equals_tag, no_position},
    {"tax_id", "TaxID", "taxid", {}, field_origin::equals_tag, no_position},
    {"rep_id", "RepID", "repid", {}, field_origin::equals_tag, no_position},
}};

// UniParc simple and proteome headers. PC distinguishes the latter.
inline constexpr std::array<header_field_definition, 9> uniparc_fields{{
    {"id", "", "id", identifier_aliases, field_origin::positional, 0},
    {"protein_name",
     "",
     "proteinname",
     protein_name_aliases,
     field_origin::description,
     no_position},
    {"status", "status", "status", {}, field_origin::equals_tag, no_position},
    {"organism_name", "OS", "organismname", {}, field_origin::equals_tag, no_position},
    {"organism_identifier", "OX", "organismidentifier", {}, field_origin::equals_tag, no_position},
    {"gene_name", "GN", "genename", {}, field_origin::equals_tag, no_position},
    {"accession_list", "AC", "accessionlist", {}, field_origin::equals_tag, no_position},
    {"source_id_list", "SS", "sourceidlist", {}, field_origin::equals_tag, no_position},
    {"proteome_component", "PC", "proteomecomponent", {}, field_origin::equals_tag, no_position},
}};

// UniMes: first-token id, name, and keyed metadata.
inline constexpr std::array<header_field_definition, 4> unimes_fields{{
    {"id", "", "id", identifier_aliases, field_origin::positional, 0},
    {"name", "", "name", description_aliases, field_origin::description, no_position},
    {"organism_name", "OS", "organismname", {}, field_origin::equals_tag, no_position},
    {"sequence_version", "SV", "sequenceversion", {}, field_origin::equals_tag, no_position},
}};

// SPD: id|gene|description with gene id and taxon extraction.
inline constexpr std::array<header_field_definition, 5> spd_fields{{
    {"id", "", "id", identifier_aliases, field_origin::positional, 0},
    {"gene", "", "gene", {}, field_origin::positional, 1},
    {"description", "", "description", {}, field_origin::positional, 2},
    {"gene_id", "", "geneid", {}, field_origin::description, no_position},
    {"taxon", "", "taxon", {}, field_origin::description, no_position},
}};

// Generic NCBI: id, description, and trailing organism.
inline constexpr std::array<header_field_definition, 4> ncbi_fields{{
    {"id", "", "id", ncbi_identifier_aliases, field_origin::positional, 0},
    {"description", "", "description", {}, field_origin::description, no_position},
    {"organism", "", "organism", {}, field_origin::bracket_tag, no_position},
    {"taxon", "", "taxon", {}, field_origin::bracket_tag, no_position},
}};

// NCBI GI: positional identifier prefix plus trailing organism.
inline constexpr std::array<header_field_definition, 5> ncbi_gi_fields{{
    {"gi", "", "gi", {}, field_origin::positional, 0},
    {"gi_number", "", "ginumber", identifier_aliases, field_origin::positional, 1},
    {"description", "", "description", {}, field_origin::description, no_position},
    {"organism", "", "organism", {}, field_origin::bracket_tag, no_position},
    {"taxon", "", "taxon", {}, field_origin::bracket_tag, no_position},
}};

// NCBI PDB: identifier prefix, chain, description, and organism.
// The chain shares a pipe-delimited field with the description. `parse_ncbi`
// splits that field, so the chain uses a named route.
inline constexpr std::array<header_field_definition, 6> ncbi_pdb_fields{{
    {"pdb", "", "pdb", {}, field_origin::positional, 0},
    {"pdb_id", "", "pdbid", accession_identifier_aliases, field_origin::positional, 1},
    {"chain", "", "chain", {}, field_origin::description, no_position},
    {"description", "", "description", {}, field_origin::description, no_position},
    {"organism", "", "organism", {}, field_origin::bracket_tag, no_position},
    {"taxon", "", "taxon", {}, field_origin::bracket_tag, no_position},
}};

// RefSeq: ref|accession| prefix, description, and trailing organism.
inline constexpr std::array<header_field_definition, 5> refseq_fields{{
    {"ref", "", "ref", {}, field_origin::positional, 0},
    {"accession", "", "accession", accession_identifier_aliases, field_origin::positional, 1},
    {"description", "", "description", {}, field_origin::description, no_position},
    {"organism", "", "organism", {}, field_origin::bracket_tag, no_position},
    {"taxon", "", "taxon", {}, field_origin::bracket_tag, no_position},
}};

// GenBank: identifier plus bracketed key-value metadata.
inline constexpr std::array<header_field_definition, 5> genbank_fields{{
    {"id", "", "id", genbank_identifier_aliases, field_origin::positional, 0},
    {"organism", "", "organism", {}, field_origin::bracket_tag, no_position},
    {"chromosome", "", "chromosome", {}, field_origin::bracket_tag, no_position},
    {"map", "", "map", {}, field_origin::bracket_tag, no_position},
    {"moltype", "", "moltype", {}, field_origin::bracket_tag, no_position},
}};

// GISAID: eight pipe-delimited positional fields.
inline constexpr std::array<header_field_definition, 8> gisaid_fields{{
    {"strain", "", "strain", name_aliases, field_origin::positional, 0},
    {"type", "", "type", {}, field_origin::positional, 1},
    {"subtype", "", "subtype", {}, field_origin::positional, 2},
    {"collection_date", "", "collectiondate", description_aliases, field_origin::positional, 3},
    {"passage", "", "passage", {}, field_origin::positional, 4},
    {"country", "", "country", {}, field_origin::positional, 5},
    {"host", "", "host", {}, field_origin::positional, 6},
    {"accession", "", "accession", identifier_aliases, field_origin::positional, 7},
}};

// IMGT: fifteen pipe-delimited positional fields.
inline constexpr std::array<header_field_definition, 15> imgt_fields{{
    {"ligm_accession",
     "",
     "ligmaccession",
     accession_identifier_aliases,
     field_origin::positional,
     0},
    {"imgt_allele_name", "", "imgtallelename", {}, field_origin::positional, 1},
    {"species", "", "species", {}, field_origin::positional, 2},
    {"functionality", "", "functionality", {}, field_origin::positional, 3},
    {"region", "", "region", {}, field_origin::positional, 4},
    {"positions", "", "positions", {}, field_origin::positional, 5},
    {"nt_count", "", "ntcount", {}, field_origin::positional, 6},
    {"codon_start", "", "codonstart", {}, field_origin::positional, 7},
    {"nt_added_5p", "", "ntadded5p", {}, field_origin::positional, 8},
    {"nt_added_removed_3p", "", "ntaddedremoved3p", {}, field_origin::positional, 9},
    {"nt_corrections", "", "ntcorrections", {}, field_origin::positional, 10},
    {"aa_count", "", "aacount", {}, field_origin::positional, 11},
    {"seq_length", "", "seqlength", {}, field_origin::positional, 12},
    {"partial", "", "partial", {}, field_origin::positional, 13},
    {"reverse_complement", "", "reversecomplement", {}, field_origin::positional, 14},
}};

inline constexpr std::array<header_field_definition, 0> no_fields{};

// Format lookup.

inline constexpr std::array<header_format_definition,
                            static_cast<std::size_t>(header_format::count)>
    header_format_definitions{{
        // format, SQL name, delimiter, metadata, fields, prefix count, synthetic recipe
        {header_format::automatic, header_format_sql_names[0], '\0', metadata_style::both, no_fields, 0,
         "{0}"},
        {header_format::plain, header_format_sql_names[1], '\0', metadata_style::none, no_fields, 0,
         "{0}"},
        {header_format::uniprotkb, header_format_sql_names[2], '|', metadata_style::equals, uniprotkb_fields, 0,
         "sp|SYN{1}|SYNTH_HUMAN {0} OS=Homo sapiens OX=9606 GN=SYNTH PE=1 SV=1"},
        {header_format::uniref, header_format_sql_names[3], '\0', metadata_style::equals, uniref_fields, 0,
         "UniRef100_SYN{1} {0} n=1 Tax=Homo sapiens TaxID=9606 RepID=SYNTH_HUMAN"},
        {header_format::uniparc, header_format_sql_names[4], '\0', metadata_style::equals, uniparc_fields, 0,
         "UPI{1:012X} {0} status=active"},
        {header_format::unimes, header_format_sql_names[5], '\0', metadata_style::equals, unimes_fields, 0,
         "MES{1} {0} OS=Homo sapiens SV=1"},
        {header_format::spd, header_format_sql_names[6], '|', metadata_style::none, spd_fields, 0,
         "SYN{1}|SYNTH_HUMAN|{0}"},
        {header_format::ncbi, header_format_sql_names[7], '\0', metadata_style::bracket, ncbi_fields, 0,
         "SYN{1} {0} [Homo sapiens]"},
        {header_format::ncbi_gi, header_format_sql_names[8], '|', metadata_style::bracket, ncbi_gi_fields, 2,
         "gi|{1}|{0} [Homo sapiens]"},
        {header_format::ncbi_pdb, header_format_sql_names[9], '|', metadata_style::bracket, ncbi_pdb_fields, 2,
         "pdb|1ABC|A {0} [Homo sapiens]"},
        {header_format::refseq, header_format_sql_names[10], '|', metadata_style::bracket, refseq_fields, 2,
         "ref|NP_{1}.1|{0} [Homo sapiens]"},
        {header_format::genbank, header_format_sql_names[11], '\0', metadata_style::bracket, genbank_fields, 0,
         "{0} [organism=Homo sapiens] [chromosome=1] [moltype=mRNA]"},
        {header_format::gisaid, header_format_sql_names[12], '|', metadata_style::none, gisaid_fields, 0,
         "A/Synthetic/{0}/{1}/2026|A|H1N1|2026-01-01|0|Poland|Human|EPI{1}"},
        {header_format::imgt, header_format_sql_names[13], '|', metadata_style::none, imgt_fields, 0,
         "{0}|IGHV1-1*01|Homo sapiens|F|V-REGION|1..64|64|1|0|0|0|21|64|partial|forward"},
    }};

/// Returns the definition for a valid format value.
/// `count` and out-of-range values are invalid.
[[nodiscard]] inline const header_format_definition&
header_format_definition_for(header_format f) noexcept {
    return header_format_definitions[static_cast<std::size_t>(f)];
}

/**
 * Format synthetic header contents from an already sampled safe component.
 * The component must be nonempty ASCII alphanumeric text. Its length excludes
 * format grammar. Identity fills numeric identifiers; auto uses plain output.
 * Returns raw header contents without a FASTA/FASTQ marker or line terminator.
 */
[[nodiscard]] inline std::string make_synthetic_header(
    header_format format, std::string_view component, std::uint64_t identity) {
    const auto& definition = header_format_definition_for(format);
    return fmt::format(fmt::runtime(definition.synthetic_pattern), component, identity);
}

// Compile-time table checks.

/// Verify that every emitted field key is the normalized canonical name.
[[nodiscard]] consteval bool all_norm_keys_canonical() {
    for (const auto& def : header_format_definitions)
        for (const auto& fd : def.fields)
            if (!normalised_equals(fd.canonical, fd.normalised_key))
                return false;
    return true;
}
static_assert(all_norm_keys_canonical(), "header field keys must match normalised canonical names");

/// Checks that each format value equals its  index in the lookup table.
[[nodiscard]] consteval bool format_table_is_dense_and_ordered() {
    for (std::size_t i = 0; i < header_format_definitions.size(); ++i)
        if (static_cast<std::size_t>(header_format_definitions[i].id) != i
            || header_format_definitions[i].sql_name != header_format_sql_names[i]
            || header_format_definitions[i].synthetic_pattern.empty())
            return false;
    return true;
}
static_assert(format_table_is_dense_and_ordered(),
              "header_format_definitions must be dense and ordered by header_format value");

// Format-name parsing.

/// Parses a format name such as 'auto' or 'uniprot'.
/// Ignores ASCII case, underscores, and hyphens. Unknown names eturn an error.
/// The error lists valid names unless the input exceeds the 64-byte limit
[[nodiscard]] inline std::expected<header_format, std::string>
parse_header_format_name(std::string_view name) {
    std::array<char, 64> scratch{};
    if (name.size() > scratch.size())
        return std::unexpected(fmt::format("unknown header_format \"{}\"", name));
    const std::string_view normalised = normalise_name(name, scratch);
    for (const auto& def : header_format_definitions) {
        if (normalised_equals(def.sql_name, normalised))
            return def.id;
    }
    std::string valid;
    for (const auto& def : header_format_definitions) {
        if (!valid.empty())
            valid += ", ";
        valid += '\'';
        valid += def.sql_name;
        valid += '\'';
    }
    return std::unexpected(
        fmt::format("unknown header_format \"{}\"; valid values are {}", name, valid));
}

} // namespace bioseq::io::identifier_parser
