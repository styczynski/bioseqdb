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

/// @brief Canonical provider identity, enumerations, dates, and bounded
/// storage.
// This is the root of the provider-core dependency chain. It imports only the
// dependency-free sink_control leaf so
// every downstream provider module (errors, model, query, capabilities, ...)
// can build on a PostgreSQL-independent, alphabet-independent foundation and
// use `sink_control` unqualified exactly as the spec writes it.
export namespace bioseq::external::provider {

// Re-export the shared continue/stop signal into the provider namespace.
using bioseq::sink_control;

// ---------------------------------------------------------------------------
// Shared byte constants (contract, not placeholders).
// ---------------------------------------------------------------------------
inline constexpr std::size_t maximum_provider_name_bytes = 64;
inline constexpr std::size_t maximum_identifier_bytes = 4096;
inline constexpr std::size_t maximum_url_bytes = 16 * 1024;
inline constexpr std::size_t maximum_archive_member_name_bytes = 4096;
inline constexpr std::size_t maximum_diagnostic_bytes = 16 * 1024;
inline constexpr std::size_t maximum_attributes_json_bytes = 1 * 1024 * 1024;

// 256-bit execution/plan identity key.
using external_execution_key = std::array<std::byte, 32>;

// ---------------------------------------------------------------------------
// Bounded owned string. `make()` rejects over-limit input before allocation so
// every owning row/diagnostic/error/URL/JSON value has a hard compile-time cap.
// ---------------------------------------------------------------------------
enum class bounded_string_error : std::uint8_t { too_long };

template <std::size_t p_maximum_size> class bounded_string {
public:
  bounded_string() = default;

  [[nodiscard]] static std::expected<bounded_string, bounded_string_error>
  make(std::string_view value) {
    if (value.size() > p_maximum_size) {
      return std::unexpected(bounded_string_error::too_long);
    }
    bounded_string result;
    result.value_.assign(value.data(), value.size());
    return result;
  }

  // Truncating factory for diagnostics/messages: never fails, clamps to cap.
  [[nodiscard]] static bounded_string make_truncated(std::string_view value) {
    bounded_string result;
    const std::size_t take =
        value.size() > p_maximum_size ? p_maximum_size : value.size();
    result.value_.assign(value.data(), take);
    return result;
  }

  [[nodiscard]] std::string_view view() const noexcept { return value_; }
  [[nodiscard]] std::size_t size() const noexcept { return value_.size(); }
  [[nodiscard]] bool is_empty() const noexcept { return value_.empty(); }
  [[nodiscard]] static constexpr std::size_t capacity_bytes() noexcept {
    return p_maximum_size;
  }
  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    return sizeof(bounded_string) + value_.capacity();
  }

  friend bool operator==(const bounded_string &lhs,
                         const bounded_string &rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }

private:
  std::string value_;
};

// ---------------------------------------------------------------------------
// Provider / policy identity. Deliberately not closed enums: every adapter
// declares a static CompTimeStr and these views point at that process-lifetime
// storage. Plans serialize the canonical string and resolve it against the
// pack.
// ---------------------------------------------------------------------------
struct provider_id {
  std::string_view value; // Points to a provider type's static CompTimeStr.
  constexpr auto operator<=>(const provider_id &) const = default;
};

struct locator_policy_id {
  std::string_view value; // Process-lifetime CompTimeStr storage.
  constexpr auto operator<=>(const locator_policy_id &) const = default;
};

struct request_policy_id {
  std::string_view value; // Process-lifetime CompTimeStr storage.
  constexpr auto operator<=>(const request_policy_id &) const = default;
};

// ---------------------------------------------------------------------------
// Enumerations.
// ---------------------------------------------------------------------------
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

enum class molecule_kind : std::uint8_t { unknown, nucleotide, protein };

enum class taxon_match : std::uint8_t { exact, descendants };

// ---------------------------------------------------------------------------
// Dates and timestamps.
// ---------------------------------------------------------------------------
struct external_date {
  std::int16_t year;
  std::uint8_t month;
  std::uint8_t day;
};

using external_timestamp = std::chrono::sys_time<std::chrono::microseconds>;

// ---------------------------------------------------------------------------
// Identity views. An accession and its version are separate values, but the
// original identifier is retained.
// ---------------------------------------------------------------------------
struct record_key_view {
  provider_id provider;
  std::string_view domain;                    // Provider leaf database/domain.
  std::optional<std::string_view> name_space; // RefSeq/GenBank/UniProtKB/ENA.
  std::string_view original_identifier;
  std::string_view accession; // Accession without version.
  std::optional<std::uint32_t> version;
};

struct genomic_interval {
  std::optional<std::string_view> assembly_accession;
  std::optional<std::string_view> assembly_database; // UCSC key, e.g. hg38.
  std::string_view sequence_alias;
  std::uint64_t start0;
  std::uint64_t end0;
  enum class strand : std::int8_t { unknown = 0, forward = 1, reverse = -1 };
  strand orientation;
};

struct sequence_origin_view {
  std::optional<std::int64_t> taxon_id;
  std::optional<std::string_view> assembly_accession;
  std::optional<std::string_view> assembly_database;
  std::optional<std::string_view> sequence_alias;
  std::optional<std::uint64_t> start0;
  std::optional<std::uint64_t> end0;
  genomic_interval::strand orientation{genomic_interval::strand::unknown};
};

// Canonical provider molecule/entity string forms (SQL text mapping).
[[nodiscard]] constexpr std::string_view
to_string(molecule_kind kind) noexcept {
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
