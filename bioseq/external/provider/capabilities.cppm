/**
 * @file external/provider/capabilities.cppm
 * @brief Provider features, planning limits, and request statistics.
 */
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

/// Provider features and statistics used during planning and retrieval.
export namespace bioseq::external::provider {

// Planning and transfer statistics.
/// Counts and completeness reported by one provider session.
struct provider_statistics {
    std::uint64_t requests{};
    std::uint64_t wire_bytes{};
    std::uint64_t decoded_bytes{};
    std::uint64_t rows{};
    std::uint64_t retries{};
    std::uint64_t warnings{};
    bool is_complete{};
};

/// Limits on query size and stored provider plans.
struct external_planning_limits {
    std::size_t maximum_predicate_nodes;
    std::size_t maximum_query_string_bytes;
    std::size_t maximum_plan_bytes_per_provider;
};

/// Estimated plan and buffer sizes before reserving execution memory.
struct provider_plan_estimate {
    std::size_t plan_bytes;
    std::size_t metadata_page_bytes;
    std::size_t staging_message_bytes;
    std::size_t transfer_count;
};

/// Request and byte counts reported by a transfer.
struct transfer_statistics {
    std::uint64_t requests{};
    std::uint64_t wire_bytes{};
    std::uint64_t decoded_bytes{};
    std::uint64_t redirects{};
    std::uint64_t resumes{};
};

// Provider features.
/// Query or delivery feature that a provider may advertise.
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

/// Provider features and limits on page size and identifier batches.
struct provider_capabilities {
    std::bitset<static_cast<std::size_t>(provider_capability::count)> flags;
    std::size_t maximum_metadata_page;
    std::size_t maximum_identifier_batch;
};

/// Borrowed capabilities for one provider domain or database.
struct domain_capability_view {
    std::string_view domain;
    std::span<const std::string_view> advertised_provider_fields;
    bool has_structured_metadata;
    bool supports_identifier_list;
    bool has_confirmed_deep_cursor;
    std::size_t maximum_page;
};

/// Borrowed capability snapshot used for planning.
struct provider_capability_snapshot {
    provider_id provider;
    std::string_view fingerprint;
    std::chrono::system_clock::time_point captured_at;
    provider_capabilities provider_level;
    std::span<const domain_capability_view> domains;
    bool is_offline_baseline;
};

/// Owning capability data for one provider domain.
struct owning_domain_capability {
    bounded_string<maximum_identifier_bytes> domain;
    std::vector<bounded_string<maximum_identifier_bytes>> advertised_provider_fields;
    bool has_structured_metadata;
    bool supports_identifier_list;
    bool has_confirmed_deep_cursor;
    std::size_t maximum_page;
    /// Estimate storage for this domain and its advertised field names.
    [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
        std::size_t total = sizeof(owning_domain_capability);
        total += domain.size();
        total += advertised_provider_fields.capacity() *
                 sizeof(bounded_string<maximum_identifier_bytes>);
        for (const auto& field : advertised_provider_fields) {
            total += field.size();
        }
        return total;
    }
};

/// Owning provider capability snapshot with bounded text fields.
struct owning_provider_capability_snapshot {
    provider_id provider;
    bounded_string<maximum_provider_name_bytes> provider_name;
    bounded_string<maximum_identifier_bytes> fingerprint;
    external_timestamp captured_at;
    provider_capabilities provider_level;
    std::vector<owning_domain_capability> domains;
    bool is_offline_baseline;
    /// Estimate storage for the snapshot and its domain data.
    [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
        std::size_t total = sizeof(owning_provider_capability_snapshot);
        total += provider_name.size();
        total += fingerprint.size();
        total += domains.capacity() * sizeof(owning_domain_capability);
        for (const owning_domain_capability& domain_capability : domains) {
            total += domain_capability.measure_deep_size_bytes() - sizeof(owning_domain_capability);
        }
        return total;
    }

    /// Borrow provider-level data, leaving the domain list empty.
    /// Domain details remain in the owning snapshot's domains member.
    [[nodiscard]] provider_capability_snapshot make_view() const noexcept {
        return provider_capability_snapshot{
            .provider = provider,
            .fingerprint = fingerprint.view(),
            .captured_at =
                std::chrono::time_point_cast<std::chrono::system_clock::duration>(captured_at),
            .provider_level = provider_level,
            .domains = {},
            .is_offline_baseline = is_offline_baseline,
        };
    }
};

} // namespace bioseq::external::provider
