/**
 * @file external/provider/base.cppm
 * @brief Shared provider IDs, bounded strings, and sequene coordinates.
 */
module;

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

export module bioseq.external.provider.base;

import bioseq.common.sink_control;

/// Shared provider IDs, dates, and values with byte limits.
// These shared types can be used without PostgreSQL or sequence alphabets.
// Import the callback result here so provider modules can  use sink_control.
export namespace bioseq::external::provider {

/// Continue-or-stop result shared by provider sink callbacks.
using bioseq::sink_control;

// Byte limits for provider data.
/// Maximum bytes kept for a provider name.
inline constexpr std::size_t maximum_provider_name_bytes = 64;
/// Maximum bytes kept for an external identifier.
inline constexpr std::size_t maximum_identifier_bytes = 4096;
/// Maximum bytes accepted for a remote locator.
inline constexpr std::size_t maximum_url_bytes = 16 * 1024;
/// Maximum bytes accepted for an archive member name.
inline constexpr std::size_t maximum_archive_member_name_bytes = 4096;
/// Maximum bytes kept for a diagnostic message.
inline constexpr std::size_t maximum_diagnostic_bytes = 16 * 1024;
/// Maximum bytes stored for serialized record attributes.
inline constexpr std::size_t maximum_attributes_json_bytes = 1 * 1024 * 1024;

/// Stable 256-bit execution and plan identity key.
using external_execution_key = std::array<std::byte, 32>;

// make() rejects text that exceeds the byte limit before copying it.
/// Error returned when bounded string construction exceeds its capacity.
enum class bounded_string_error : std::uint8_t { too_long };

/// Owning string whose accepted length is capped at compile time.
template <std::size_t p_maximum_size> class bounded_string {
  public:
    /// Construct an empty bounded string.
    bounded_string() = default;

    /// Copy a value when it fits within the byte capacity.
    [[nodiscard]] static std::expected<bounded_string, bounded_string_error>
    make(std::string_view value) {
        if (value.size() > p_maximum_size) {
            return std::unexpected(bounded_string_error::too_long);
        }
        bounded_string result;
        result.value_.assign(value.data(), value.size());
        return result;
    }

    /// Copy at most the byte capacity, which can split a UTF-8 character.
    [[nodiscard]] static bounded_string make_truncated(std::string_view value) {
        bounded_string result;
        const std::size_t take = value.size() > p_maximum_size ? p_maximum_size : value.size();
        result.value_.assign(value.data(), take);
        return result;
    }

    /// Return a non-owning view of the stored bytes.
    [[nodiscard]] std::string_view view() const noexcept {
        return value_;
    }
    /// Return the stored byte count.
    [[nodiscard]] std::size_t size() const noexcept {
        return value_.size();
    }
    /// Return whether no bytes are stored.
    [[nodiscard]] bool is_empty() const noexcept {
        return value_.empty();
    }
    /// Return the compile-time byte capacity.
    [[nodiscard]] static constexpr std::size_t capacity_bytes() noexcept {
        return p_maximum_size;
    }
    /// Estimate storage from the object size and string capacity.
    [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
        return sizeof(bounded_string) + value_.capacity();
    }

    friend bool operator==(const bounded_string& lhs, const bounded_string& rhs) noexcept {
        return lhs.value_ == rhs.value_;
    }

  private:
    std::string value_;
};

// IDs borrow names that must remain valid for the  process lifetime.
// Plans store these names and resolve them against configured providers.
/// Provider name borrowed from static storage.
struct provider_id {
    std::string_view value; // Provider name in static storage.
    constexpr auto operator<=>(const provider_id&) const = default;
};

/// Locator policy name borrowed from static storage.
struct locator_policy_id {
    std::string_view value; // Policy name in static storage.
    constexpr auto operator<=>(const locator_policy_id&) const = default;
};

/// Request policy name borrowed from static storage
struct request_policy_id {
    std::string_view value; // Policy, name in static storage.
    constexpr auto operator<=>(const request_policy_id&) const = default;
};

// Record categories.
/// Biological entity represented by an external record.
enum class entity_kind : std::uint8_t {
    taxon,
    assembly,
    assembly_sequence,
    gene,
    transcript,
    cds,
    nucleotide,
    protein,
    virus_sequence,
    organelle_sequence
};

/// Whether the sequenc contains nucleotides or proteins, or is unknown.
enum class molecule_kind : std::uint8_t { unknown, nucleotide, protein };

/// Match the named taxon alone or include its descendants.
enum class taxon_match : std::uint8_t { exact, descendants };

// Dates and timestamps.
/// Calendar date without a time zone or time of day.
struct external_date {
    std::int16_t year;
    std::uint8_t month;
    std::uint8_t day;
};

/// UTC provider timestamp represented to microsecond precision.
using external_timestamp = std::chrono::sys_time<std::chrono::microseconds>;

// Keep  the original record identifier alongside its accession and version.
/// Borrowed provider record identity with accession and optional version.
struct record_key_view {
    provider_id provider;
    std::string_view domain;                    // Database within the provider.
    std::optional<std::string_view> name_space; // RefSeq/GenBank/UniProtKB/ENA.
    std::string_view original_identifier;
    std::string_view accession; // Accession without version.
    std::optional<std::uint32_t> version;
};

/// Zero-based half-open interval on a provider sequence alias.
struct genomic_interval {
    std::optional<std::string_view> assembly_accession;
    std::optional<std::string_view> assembly_database; // UCSC key, e.g. hg38.
    std::string_view sequence_alias;
    std::uint64_t start0;
    std::uint64_t end0;
    /// Orientation of a genomic interval.
    enum class strand : std::int8_t { unknown = 0, forward = 1, reverse = -1 };
    strand orientation;
};

/// Sequence source and optional coordinates, with borrowed names.
struct sequence_origin_view {
    std::optional<std::int64_t> taxon_id;
    std::optional<std::string_view> assembly_accession;
    std::optional<std::string_view> assembly_database;
    std::optional<std::string_view> sequence_alias;
    std::optional<std::uint64_t> start0;
    std::optional<std::uint64_t> end0;
    genomic_interval::strand orientation{genomic_interval::strand::unknown};
};

/// Return the canonical SQL text for a molecule category.
[[nodiscard]] constexpr std::string_view to_string(molecule_kind kind) noexcept {
    switch (kind) {
    case molecule_kind::nucleotide:
        return "nucleotide";
    case molecule_kind::protein:
        return "protein";
    case molecule_kind::unknown:
        return "unknown";
    }
    return "unknown";
}

/// Return the canonical SQL text for an entity category.
[[nodiscard]] constexpr std::string_view to_string(entity_kind kind) noexcept {
    switch (kind) {
    case entity_kind::taxon:
        return "taxon";
    case entity_kind::assembly:
        return "assembly";
    case entity_kind::assembly_sequence:
        return "assembly_sequence";
    case entity_kind::gene:
        return "gene";
    case entity_kind::transcript:
        return "transcript";
    case entity_kind::cds:
        return "cds";
    case entity_kind::nucleotide:
        return "nucleotide";
    case entity_kind::protein:
        return "protein";
    case entity_kind::virus_sequence:
        return "virus_sequence";
    case entity_kind::organelle_sequence:
        return "organelle_sequence";
    }
    return "nucleotide";
}

} // namespace bioseq::external::provider
