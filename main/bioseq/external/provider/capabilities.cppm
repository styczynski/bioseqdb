module;

#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

export module bioseq.external.provider.capabilities;

import bioseq.external.provider.base;

/// @brief Provider capability contracts and planning/transfer statistics.
export namespace bioseq::external::provider {

// ---------------------------------------------------------------------------
// Planning and transfer statistics.
// ---------------------------------------------------------------------------
struct provider_statistics {
  std::uint64_t requests{};
  std::uint64_t wire_bytes{};
  std::uint64_t decoded_bytes{};
  std::uint64_t rows{};
  std::uint64_t retries{};
  std::uint64_t warnings{};
  bool is_complete{};
};

struct external_planning_limits {
  std::size_t maximum_predicate_nodes;
  std::size_t maximum_query_string_bytes;
  std::size_t maximum_plan_bytes_per_provider;
};

struct provider_plan_estimate {
  std::size_t plan_bytes;
  std::size_t metadata_page_bytes;
  std::size_t staging_message_bytes;
  std::size_t transfer_count;
};

struct transfer_statistics {
  std::uint64_t requests{};
  std::uint64_t wire_bytes{};
  std::uint64_t decoded_bytes{};
  std::uint64_t redirects{};
  std::uint64_t resumes{};
};

// ---------------------------------------------------------------------------
// Capabilities.
// ---------------------------------------------------------------------------
enum class provider_capability : std::uint8_t {
  taxonomy_search,
  catalog_search,
  record_gene_projection,
  record_location_projection,
  direct_inspect,
  nucleotide_locator,
  protein_locator,
  exact_taxon,
  descendant_taxon,
  cursor_paging,
  bulk_export,
  count
};

struct provider_capabilities {
  std::bitset<static_cast<std::size_t>(provider_capability::count)> flags;
  std::size_t maximum_metadata_page;
  std::size_t maximum_identifier_batch;
};

struct domain_capability_view {
  std::string_view domain;
  std::span<const std::string_view> advertised_provider_fields;
  bool has_structured_metadata;
  bool supports_identifier_list;
  bool has_confirmed_deep_cursor;
  std::size_t maximum_page;
};

struct provider_capability_snapshot {
  provider_id provider;
  std::string_view fingerprint;
  std::chrono::system_clock::time_point captured_at;
  provider_capabilities provider_level;
  std::span<const domain_capability_view> domains;
  bool is_offline_baseline;
};

struct owning_domain_capability {
  bounded_string<maximum_identifier_bytes> domain;
  std::vector<bounded_string<maximum_identifier_bytes>>
      advertised_provider_fields;
  bool has_structured_metadata;
  bool supports_identifier_list;
  bool has_confirmed_deep_cursor;
  std::size_t maximum_page;
  /// @brief Measures owned memory including dynamic allocations.
  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    std::size_t total = sizeof(owning_domain_capability);
    total += domain.size();
    total += advertised_provider_fields.capacity() *
             sizeof(bounded_string<maximum_identifier_bytes>);
    for (const auto &field : advertised_provider_fields) {
      total += field.size();
    }
    return total;
  }
};

struct owning_provider_capability_snapshot {
  provider_id provider;
  bounded_string<maximum_provider_name_bytes> provider_name;
  bounded_string<maximum_identifier_bytes> fingerprint;
  external_timestamp captured_at;
  provider_capabilities provider_level;
  std::vector<owning_domain_capability> domains;
  bool is_offline_baseline;
  /// @brief Measures owned memory including dynamic allocations.
  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    std::size_t total = sizeof(owning_provider_capability_snapshot);
    total += provider_name.size();
    total += fingerprint.size();
    total += domains.capacity() * sizeof(owning_domain_capability);
    for (const owning_domain_capability &domain_capability : domains) {
      total += domain_capability.measure_deep_size_bytes() -
               sizeof(owning_domain_capability);
    }
    return total;
  }

  /// @brief Projects provider-level capability data into a borrowed view.
  ///
  /// Domain detail remains available through `domains`; returning a span
  /// would require separate backing storage for converted domain views.
  [[nodiscard]] provider_capability_snapshot make_view() const noexcept {
    return provider_capability_snapshot{
        .provider = provider,
        .fingerprint = fingerprint.view(),
        .captured_at =
            std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                captured_at),
        .provider_level = provider_level,
        .domains = {},
        .is_offline_baseline = is_offline_baseline,
    };
  }
};

} // namespace bioseq::external::provider
