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

// DOC0372 — Provider capability contract plus planning/transfer statistics. The
// planner reads an immutable capability snapshot from the shared cache; the
// owning snapshot is what the cache stores (through a shared_ptr) and projects
// into a non-owning view for the frozen plan.
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
    bool complete{};
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
    bool structured_metadata;
    bool identifier_list;
    bool deep_cursor_confirmed;
    std::size_t maximum_page;
};

struct provider_capability_snapshot {
    provider_id provider;
    std::string_view fingerprint;
    std::chrono::system_clock::time_point captured_at;
    provider_capabilities provider_level;
    std::span<const domain_capability_view> domains;
    bool offline_baseline;
};

struct owning_domain_capability {
    bounded_string<maximum_identifier_bytes> domain;
    std::vector<bounded_string<maximum_identifier_bytes>>
        advertised_provider_fields;
    bool structured_metadata;
    bool identifier_list;
    bool deep_cursor_confirmed;
    std::size_t maximum_page;
    std::size_t deep_size_bytes() const noexcept;
};

inline std::size_t owning_domain_capability::deep_size_bytes() const noexcept {
    std::size_t total = sizeof(owning_domain_capability);
    total += domain.size();
    total += advertised_provider_fields.capacity() *
             sizeof(bounded_string<maximum_identifier_bytes>);
    for (const auto& field : advertised_provider_fields) {
        total += field.size();
    }
    return total;
}

struct owning_provider_capability_snapshot {
    provider_id provider;
    bounded_string<maximum_provider_name_bytes> provider_name;
    bounded_string<maximum_identifier_bytes> fingerprint;
    external_timestamp captured_at;
    provider_capabilities provider_level;
    std::vector<owning_domain_capability> domains;
    bool offline_baseline;
    std::size_t deep_size_bytes() const noexcept;
    provider_capability_snapshot view() const noexcept;
};

inline std::size_t
owning_provider_capability_snapshot::deep_size_bytes() const noexcept {
    std::size_t total = sizeof(owning_provider_capability_snapshot);
    total += provider_name.size();
    total += fingerprint.size();
    total += domains.capacity() * sizeof(owning_domain_capability);
    for (const owning_domain_capability& entry : domains) {
        total += entry.deep_size_bytes() - sizeof(owning_domain_capability);
    }
    return total;
}

// Provider-level projection of the owning snapshot. The owning object (held by
// the capability cache through a shared_ptr) outlives the returned view, so the
// borrowed provider/fingerprint members reference its storage directly. The
// per-domain view span is intentionally left empty: materializing the
// domain_capability_view/std::string_view arrays it would point at requires
// backing storage this non-allocating accessor does not own, so callers needing
// domain detail read the owning `domains` vector.
inline provider_capability_snapshot
owning_provider_capability_snapshot::view() const noexcept {
    return provider_capability_snapshot{
        .provider = provider,
        .fingerprint = fingerprint.view(),
        .captured_at = std::chrono::time_point_cast<
            std::chrono::system_clock::duration>(captured_at),
        .provider_level = provider_level,
        .domains = {},
        .offline_baseline = offline_baseline,
    };
}

}  // namespace bioseq::external::provider
