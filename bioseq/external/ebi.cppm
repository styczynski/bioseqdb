/**
 * @file external/ebi.cppm
 * @brief Assembles the OpenAPI document used to generate the EBI Search client.
 *
 * The fragment order joins the document header, paths, schemas, and Bioseq
 * client settings into one string. The  generator uses this module as the
 * client's source file
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

export module bioseq.external.ebi;

import :document;
import :search_paths;
import :entry_paths;
import :notification_paths;
import :sequence_tool_paths;
import :result_schemas;
import :support_schemas;
import :integration;

/** Sums the byte lengths of the supplied descriptor fragments. */
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
 * Joins descriptor fragments in order and adds a terminating zero byte.
 *
 * p_payload_size must equal the combined fragment length. The returned array
 * owns the document bytes and its terminator.
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

/** OpenAPI fragments in the order required to fmrm the complete document. */
static constexpr auto spec_fragments = std::to_array<std::string_view>({
    bioseq::external::ebi::descriptor::document_preamble,
    bioseq::external::ebi::descriptor::autocomplete_path,
    bioseq::external::ebi::descriptor::entry_lookup_paths,
    bioseq::external::ebi::descriptor::log_path,
    bioseq::external::ebi::descriptor::related_entry_paths,
    bioseq::external::ebi::descriptor::search_paths,
    bioseq::external::ebi::descriptor::mail_path,
    bioseq::external::ebi::descriptor::sequence_tool_paths,
    bioseq::external::ebi::descriptor::top_terms_path,
    bioseq::external::ebi::descriptor::result_schemas,
    bioseq::external::ebi::descriptor::support_schemas,
    bioseq::external::ebi::descriptor::integration_contracts,
});

/** @brief Null-terminated storage for the composed OpenAPI document. */
static constexpr auto spec_storage =
    concatenate_descriptor<descriptor_size(spec_fragments)>(spec_fragments);

/** Complete OpenAPI text, excluding its terminating zero byte. */
static constexpr std::string_view spec{spec_storage.data(), spec_storage.size() - 1};

/** Declaration whose attributes request generation of the EBI client. */
[[bioseq::api_client(spec)]] [[bioseq::api_client_fragments(
    spec_fragments)]] export struct bioseq_external_api_client;
