/**
 * @file external/ncbi.cppm
 * @brief NCBI Datasets OpenAPI descriptor assembled from provider partitions.
 *
 * This primary module is the generated client source key.
 * The spec_fragments declaration defines the complete descriptor byte sequence.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

export module bioseq.external.ncbi;

import :document;
import :genome_paths;
import :biosample_paths;
import :gene_paths;
import :protein_paths;
import :taxonomy_paths;
import :organelle_paths;
import :virus_paths;
import :service_schemas;
import :report_schemas;
import :specialist_schemas;
import :value_schemas;
import :integration;

/**
 * @brief Counts bytes in an ordered descriptor fragment list.
 * @tparam p_fragment_count Number of fragments in the list.
 * @param fragments Fragment views in composition order.
 * @return Sum of the fragment byte counts.
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
 * @brief Copies ordered descriptor fragments into null-terminated storage.
 * @tparam p_payload_size Combined descriptor byte count.
 * @tparam p_fragment_count Number of fragments in the list.
 * @param fragments Fragment views in composition order.
 * @return Constant storage containing every fragment in order.
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

/** @brief Raw descriptor fragments in parser and composition order. */
static constexpr auto spec_fragments = std::to_array<std::string_view>({
    bioseq::external::ncbi::descriptor::document_preamble,
    bioseq::external::ncbi::descriptor::genome_catalog_paths,
    bioseq::external::ncbi::descriptor::biosample_report_path,
    bioseq::external::ncbi::descriptor::genome_download_paths,
    bioseq::external::ncbi::descriptor::gene_download_paths,
    bioseq::external::ncbi::descriptor::protein_download_paths,
    bioseq::external::ncbi::descriptor::genome_annotation_download_path,
    bioseq::external::ncbi::descriptor::taxonomy_download_paths,
    bioseq::external::ncbi::descriptor::genome_check_paths,
    bioseq::external::ncbi::descriptor::organelle_download_paths,
    bioseq::external::ncbi::descriptor::version_path,
    bioseq::external::ncbi::descriptor::gene_report_paths,
    bioseq::external::ncbi::descriptor::genome_annotation_paths,
    bioseq::external::ncbi::descriptor::organelle_report_paths,
    bioseq::external::ncbi::descriptor::taxonomy_report_paths,
    bioseq::external::ncbi::descriptor::virus_report_paths,
    bioseq::external::ncbi::descriptor::genome_revision_path,
    bioseq::external::ncbi::descriptor::service_schemas,
    bioseq::external::ncbi::descriptor::report_schemas,
    bioseq::external::ncbi::descriptor::specialist_schemas,
    bioseq::external::ncbi::descriptor::value_schemas,
    bioseq::external::ncbi::descriptor::integration_contracts,
});

/** @brief Null-terminated storage for the composed OpenAPI document. */
static constexpr auto spec_storage =
    concatenate_descriptor<descriptor_size(spec_fragments)>(spec_fragments);

/** @brief Exact OpenAPI document consumed by NCBI Datasets client generation. */
static constexpr std::string_view spec{spec_storage.data(), spec_storage.size() - 1};

/** @brief Code-generation anchor for the NCBI Datasets client. */
[[bioseq::api_client(spec)]] [[bioseq::api_client_fragments(
    spec_fragments)]] export struct bioseq_external_api_client;
