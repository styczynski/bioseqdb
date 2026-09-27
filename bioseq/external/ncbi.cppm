/**
 * @file external/ncbi.cppm
 * @brief Assembles the OpenAPI document used to generate the NCBI client.
 *
 * The ordered spec_fragments list joins API metadata, endpoint definitions,
 * and data types from the module partitions into one document.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

export module bioseq.external.ncbi;

import :document;
import :genome_accession_paths;
import :genome_project_sample_paths;
import :genome_name_paths;
import :genome_sequence_paths;
import :genome_paths;
import :biosample_paths;
import :gene_paths;
import :gene_identity_paths;
import :gene_dataset_report_paths;
import :gene_product_report_paths;
import :gene_summary_paths;
import :protein_paths;
import :taxonomy_paths;
import :organelle_paths;
import :virus_paths;
import :virus_report_paths;
import :virus_download_paths;
import :service_schemas;
import :service_taxonomy_virus_schemas;
import :report_schemas;
import :gene_report_schemas;
import :taxonomy_report_schemas;
import :specialist_schemas;
import :value_schemas;
import :integration;

/**
 * @brief Counts the bytes in the OpenAPI text parts.
 * @tparam p_fragment_count Number of parts.
 * @param fragments Text parts in document order.
 * @return Total bytes, excluding a final null terminator.
 */
template <std::size_t p_fragment_count>
[[nodiscard]] consteval std::size_t
descriptor_size(const std::array<std::string_view, p_fragment_count>& fragments) noexcept {
    std::size_t size{};
    for (const std::string_view& fragment : fragments) {
        size += fragment.size();
    }
    return size;
}

/**
 * @brief Joins OpenAPI text parts into null-terminated storage.
 * @tparam p_payload_size Total text size, excluding the null terminator.
 * @tparam p_fragment_count Number of parts.
 * @param fragments Text parts in document order.
 * @return The combined text followed by a null byte.
 */
template <std::size_t p_payload_size, std::size_t p_fragment_count>
[[nodiscard]] consteval auto
concatenate_descriptor(const std::array<std::string_view, p_fragment_count>& fragments) noexcept {
    std::array<char, p_payload_size + 1> descriptor{};
    char* output = descriptor.data();
    for (const std::string_view& fragment : fragments) {
        output = std::ranges::copy(fragment, output).out;
    }
    return descriptor;
}

/** @brief OpenAPI text parts in the order required to form one YAML document. */
static constexpr auto spec_fragments = std::to_array<std::string_view>({
    bioseq::external::ncbi::descriptor::document_preamble,
    bioseq::external::ncbi::descriptor::genome_accession_catalog_paths,
    bioseq::external::ncbi::descriptor::genome_project_sample_catalog_paths,
    bioseq::external::ncbi::descriptor::genome_name_catalog_paths,
    bioseq::external::ncbi::descriptor::genome_sequence_catalog_paths,
    bioseq::external::ncbi::descriptor::biosample_report_path,
    bioseq::external::ncbi::descriptor::genome_download_paths,
    bioseq::external::ncbi::descriptor::gene_download_paths,
    bioseq::external::ncbi::descriptor::protein_download_paths,
    bioseq::external::ncbi::descriptor::genome_annotation_download_path,
    bioseq::external::ncbi::descriptor::taxonomy_download_paths,
    bioseq::external::ncbi::descriptor::genome_check_paths,
    bioseq::external::ncbi::descriptor::organelle_download_paths,
    bioseq::external::ncbi::descriptor::version_path,
    bioseq::external::ncbi::descriptor::gene_identity_paths,
    bioseq::external::ncbi::descriptor::gene_dataset_report_paths,
    bioseq::external::ncbi::descriptor::gene_product_report_paths,
    bioseq::external::ncbi::descriptor::gene_summary_paths,
    bioseq::external::ncbi::descriptor::genome_annotation_paths,
    bioseq::external::ncbi::descriptor::organelle_report_paths,
    bioseq::external::ncbi::descriptor::taxonomy_report_paths,
    bioseq::external::ncbi::descriptor::virus_summary_paths,
    bioseq::external::ncbi::descriptor::virus_report_paths,
    bioseq::external::ncbi::descriptor::virus_download_paths,
    bioseq::external::ncbi::descriptor::genome_revision_path,
    bioseq::external::ncbi::descriptor::service_core_schemas,
    bioseq::external::ncbi::descriptor::service_taxonomy_virus_schemas,
    bioseq::external::ncbi::descriptor::assembly_biosample_report_schemas,
    bioseq::external::ncbi::descriptor::gene_protein_report_schemas,
    bioseq::external::ncbi::descriptor::sequence_taxonomy_virus_report_schemas,
    bioseq::external::ncbi::descriptor::specialist_schemas,
    bioseq::external::ncbi::descriptor::value_schemas,
    bioseq::external::ncbi::descriptor::integration_contracts,
});

/** @brief Null-terminated storage for the composed OpenAPI document. */
static constexpr auto spec_storage =
    concatenate_descriptor<descriptor_size(spec_fragments)>(spec_fragments);

/** @brief The assembled OpenAPI text passed to NCBI client generation. */
static constexpr std::string_view spec{spec_storage.data(), spec_storage.size() - 1};

/** @brief Declaration marked for NCBI client generation. */
[[bioseq::api_client(spec)]] [[bioseq::api_client_fragments(
    spec_fragments)]] export struct bioseq_external_api_client;
