/**
 * @file external/uniprot.cppm
 * @brief Assemble the OpenAPI description used to generate the UniProt client.
 *
 * The generator reads the fragments in spec_fragments order and identifies
 * the client by this primary module.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

export module bioseq.external.uniprot;

import :document;
import :operations;
import :entry_schemas;
import :xml_schemas;
import :search_schemas;
import :annotation_schemas;
import :publication_schemas;
import :integration;

/**
 * @brief Count the bytes in the OpenAPI fragments.
 * @tparam p_fragment_count Number of fragments.
 * @param fragments Fragments in document order.
 * @return Total number of bytes in the fragments.
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
 * @brief Join OpenAPI fragments in null-terminated storage.
 * @tparam p_payload_size Combined size of the fragment contents.
 * @tparam p_fragment_count Number of fragments.
 * @param fragments Fragments in document order.
 * @return Constant array containing the fragments followed by a zero byte.
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

/** @brief OpenAPI fragments in the order they appear in the document. */
static constexpr auto spec_fragments = std::to_array<std::string_view>({
    bioseq::external::uniprot::descriptor::document_preamble,
    bioseq::external::uniprot::descriptor::operation_paths,
    bioseq::external::uniprot::descriptor::entry_schemas,
    bioseq::external::uniprot::descriptor::xml_schemas,
    bioseq::external::uniprot::descriptor::search_schemas,
    bioseq::external::uniprot::descriptor::annotation_schemas,
    bioseq::external::uniprot::descriptor::publication_schemas,
    bioseq::external::uniprot::descriptor::integration_contracts,
});

/** @brief Null-terminated storage for the composed OpenAPI document. */
static constexpr auto spec_storage =
    concatenate_descriptor<descriptor_size(spec_fragments)>(spec_fragments);

/** @brief Complete OpenAPI document  used to generate the UniProt client. */
static constexpr std::string_view spec{spec_storage.data(), spec_storage.size() - 1};

/** @brief Generate the UniProt client from the listed OpenAPI fragments. */
[[bioseq::api_client(spec)]] [[bioseq::api_client_fragments(
    spec_fragments)]] export struct bioseq_external_api_client;
