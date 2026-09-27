/**
 * @file external/provider/messages.cppm
 * @brief Provider results and download plans passed between workers.
 */
module;

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

export module bioseq.external.provider.messages;

import bioseq.external.provider.base;
import bioseq.external.provider.errors;
import bioseq.external.provider.model;
import bioseq.common.sink_control;

/// Provider messages and the views used to read them.
///
/// Views borrow their data for one callback. Owning messages keep that data
/// alive while it passes through a channel.
export namespace bioseq::external::provider {

// Diagnostics.

/// Severity of a provider diagnostic that does not abort the whole query.
enum class diagnostic_severity : std::uint8_t { warning, item_error };

/// Diagnostic whose text is borrowed for one callback.
struct provider_diagnostic_view {
    diagnostic_severity severity;
    provider_id provider;
    std::string_view operation;
    std::string_view item_id;
    std::string_view code;
    std::string_view message;
    bool is_retryable;
};

/// Diagnostic that owns its text within fixed byte limits.
struct owning_provider_diagnostic {
    diagnostic_severity severity;
    provider_id provider;
    bounded_string<maximum_provider_name_bytes> provider_name;
    bounded_string<maximum_identifier_bytes> operation;
    bounded_string<maximum_identifier_bytes> item_id;
    bounded_string<256> code;
    bounded_string<maximum_diagnostic_bytes> message;
    bool is_retryable{};

    /// Estimate storage for the diagnostic and its text.
    [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
        std::size_t total = sizeof(*this);
        total += provider_name.measure_deep_size_bytes() - sizeof(provider_name);
        total += operation.measure_deep_size_bytes() - sizeof(operation);
        total += item_id.measure_deep_size_bytes() - sizeof(item_id);
        total += code.measure_deep_size_bytes() - sizeof(code);
        total += message.measure_deep_size_bytes() - sizeof(message);
        return total;
    }

    /// Borrow this diagnostic's text for a callback.
    [[nodiscard]] provider_diagnostic_view view() const noexcept {
        provider_diagnostic_view result;
        result.severity = severity;
        result.provider = provider;
        result.operation = operation.view();
        result.item_id = item_id.view();
        result.code = code.view();
        result.message = message.view();
        result.is_retryable = is_retryable;
        return result;
    }
};

// Download plans borrowed for one callback.
// locator_policy_id is defined in bioseq.external.provider.base.

/// HTTP method used for a transfer candidate.
enum class transfer_method : std::uint8_t { get, post };

/// URL scheme accepted by a locator policy.
enum class locator_scheme : std::uint8_t { https, http, ftps, ftp };

/// Download request whose URL, body, and metadata are borrowed.
struct candidate_transfer_request_view {
    transfer_method method;
    std::string_view url;
    std::optional<std::string_view> accept;
    std::optional<std::string_view> content_type;
    std::span<const std::byte> immutable_body;
    bool is_replayable;
    bool is_idempotent;
    std::optional<std::uint64_t> expected_wire_bytes;
    std::optional<std::string_view> etag;
    std::optional<std::string_view> last_modified;
    std::optional<std::string_view> archive_member;
    std::optional<std::string_view> media_type;
    std::optional<std::string_view> compression;
    std::optional<std::string_view> container;
    std::optional<integrity_check_view> integrity;
};

/// How to retrieve a sequence file or collection of records.
enum class retrieval_plan_kind : std::uint8_t {
    direct_asset,
    archive_manifest,
    dehydrated_manifest,
    bounded_endpoint,
    bulk_export,
    batch_endpoint
};

/// Biological sequence content requested from an archive.
enum class sequence_content_kind : std::uint8_t {
    genomic_fasta,
    transcript_fasta,
    cds_fasta,
    protein_fasta
};

/// Borrowed rules for choosing sequence content from an archive.
struct retrieval_manifest_rules_view {
    std::span<const sequence_content_kind> selected_content;
    std::optional<std::string_view> dataset_catalog_member;
    std::optional<std::string_view> fetch_manifest_member;
    std::optional<std::string_view> checksum_manifest_member;
};

/// Borrowed identity and expected properties of one requested sequence.
struct retrieval_record_request_view {
    record_key_view key;
    molecule_kind molecule;
    sequence_origin_view origin;
    std::optional<std::uint64_t> expected_length;
    std::span<const record_key_view> accepted_header_aliases;
};

/// Rule used to associate decoded records with requested identities.
enum class record_association_rule : std::uint8_t {
    exact_single,
    fasta_header_accession,
    manifest_catalog
};

/// Download plan borrowed for one callback.
struct sequence_retrieval_plan_view {
    external_execution_key execution;
    provider_id provider;
    std::optional<record_key_view> container_key;
    std::span<const retrieval_record_request_view> requested_records;
    record_association_rule association;
    retrieval_plan_kind kind;
    locator_policy_id locator_policy;
    std::span<const candidate_transfer_request_view> candidates;
    std::optional<retrieval_manifest_rules_view> manifest;
    row_audit_view audit;
};

// Download plans with owned storage.

/// Download request that owns its URL, body, and metadata.
/// Credentials and secret headers must be supplied separately.
struct owning_candidate_transfer_request {
    transfer_method method{};
    bounded_string<maximum_url_bytes> url;
    std::optional<bounded_string<maximum_identifier_bytes>> accept;
    std::optional<bounded_string<maximum_identifier_bytes>> content_type;
    std::vector<std::byte> immutable_body;
    bool is_replayable{};
    bool is_idempotent{};
    std::optional<std::uint64_t> expected_wire_bytes;
    std::optional<bounded_string<maximum_identifier_bytes>> etag;
    std::optional<bounded_string<maximum_identifier_bytes>> last_modified;
    std::optional<bounded_string<maximum_archive_member_name_bytes>> archive_member;
    std::optional<bounded_string<256>> media_type;
    std::optional<bounded_string<maximum_identifier_bytes>> compression;
    std::optional<bounded_string<maximum_identifier_bytes>> container;
    std::optional<owning_integrity_check> integrity;

    /// Estimate storage for the request body and metadata.
    [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
        std::size_t total = sizeof(*this);
        total += url.measure_deep_size_bytes() - sizeof(url);
        if (accept)
            total += accept->measure_deep_size_bytes() - sizeof(*accept);
        if (content_type) {
            total += content_type->measure_deep_size_bytes() - sizeof(*content_type);
        }
        total += immutable_body.capacity() * sizeof(std::byte);
        if (etag)
            total += etag->measure_deep_size_bytes() - sizeof(*etag);
        if (last_modified) {
            total += last_modified->measure_deep_size_bytes() - sizeof(*last_modified);
        }
        if (archive_member) {
            total += archive_member->measure_deep_size_bytes() - sizeof(*archive_member);
        }
        if (media_type) {
            total += media_type->measure_deep_size_bytes() - sizeof(*media_type);
        }
        if (compression) {
            total += compression->measure_deep_size_bytes() - sizeof(*compression);
        }
        if (container) {
            total += container->measure_deep_size_bytes() - sizeof(*container);
        }
        if (integrity) {
            total += integrity->measure_deep_size_bytes() - sizeof(*integrity);
        }
        return total;
    }

    /// Borrow the request body and metadata for a callback.
    [[nodiscard]] candidate_transfer_request_view view() const noexcept {
        candidate_transfer_request_view result;
        result.method = method;
        result.url = url.view();
        if (accept)
            result.accept = accept->view();
        if (content_type)
            result.content_type = content_type->view();
        result.immutable_body =
            std::span<const std::byte>(immutable_body.data(), immutable_body.size());
        result.is_replayable = is_replayable;
        result.is_idempotent = is_idempotent;
        result.expected_wire_bytes = expected_wire_bytes;
        if (etag)
            result.etag = etag->view();
        if (last_modified)
            result.last_modified = last_modified->view();
        if (archive_member)
            result.archive_member = archive_member->view();
        if (media_type)
            result.media_type = media_type->view();
        if (compression)
            result.compression = compression->view();
        if (container)
            result.container = container->view();
        if (integrity)
            result.integrity = integrity->view();
        return result;
    }
};

/// Archive content selection with owned member names.
struct owning_retrieval_manifest_rules {
    std::vector<sequence_content_kind> selected_content;
    std::optional<bounded_string<maximum_archive_member_name_bytes>> dataset_catalog_member;
    std::optional<bounded_string<maximum_archive_member_name_bytes>> fetch_manifest_member;
    std::optional<bounded_string<maximum_archive_member_name_bytes>> checksum_manifest_member;

    /// Estimate storage for content selections and manifest member names.
    [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
        std::size_t total = sizeof(*this);
        total += selected_content.capacity() * sizeof(sequence_content_kind);
        if (dataset_catalog_member) {
            total +=
                dataset_catalog_member->measure_deep_size_bytes() - sizeof(*dataset_catalog_member);
        }
        if (fetch_manifest_member) {
            total +=
                fetch_manifest_member->measure_deep_size_bytes() - sizeof(*fetch_manifest_member);
        }
        if (checksum_manifest_member) {
            total += checksum_manifest_member->measure_deep_size_bytes() -
                     sizeof(*checksum_manifest_member);
        }
        return total;
    }

    /// Borrow these archive selection rules for a callback.
    [[nodiscard]] retrieval_manifest_rules_view view() const noexcept {
        retrieval_manifest_rules_view result;
        result.selected_content = std::span<const sequence_content_kind>(selected_content.data(),
                                                                         selected_content.size());
        if (dataset_catalog_member) {
            result.dataset_catalog_member = dataset_catalog_member->view();
        }
        if (fetch_manifest_member) {
            result.fetch_manifest_member = fetch_manifest_member->view();
        }
        if (checksum_manifest_member) {
            result.checksum_manifest_member = checksum_manifest_member->view();
        }
        return result;
    }
};

/// Requested sequence with owned identity and accepted FASTA header aliases.
/// The parent plan builds views backed by this alias storage.
struct owning_retrieval_record_request {
    owning_record_key key;
    molecule_kind molecule{};
    owning_sequence_origin origin;
    std::optional<std::uint64_t> expected_length;
    std::vector<owning_record_key> accepted_header_aliases;
    owning_sequence_audit_seed audit_seed;

    /// Estimate storage for the record identity, aliases, and audit data.
    [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
        std::size_t total = sizeof(*this);
        total += key.measure_deep_size_bytes() - sizeof(key);
        total += origin.measure_deep_size_bytes() - sizeof(origin);
        total += accepted_header_aliases.capacity() * sizeof(owning_record_key);
        for (const auto& alias : accepted_header_aliases) {
            total += alias.measure_deep_size_bytes() - sizeof(alias);
        }
        total += audit_seed.measure_deep_size_bytes() - sizeof(audit_seed);
        return total;
    }
};

/// Download plan that owns its data and temporary callback views.
/// Keep each view within its callback because the next view rebuilds that storage.
struct owning_sequence_retrieval_plan {
    external_execution_key execution{};
    provider_id provider;
    std::optional<owning_record_key> container_key;
    std::vector<owning_retrieval_record_request> requested_records;
    record_association_rule association{};
    retrieval_plan_kind kind{};
    locator_policy_id locator_policy;
    std::vector<owning_candidate_transfer_request> candidates;
    std::optional<owning_retrieval_manifest_rules> manifest;
    owning_row_audit audit;

    // Temporary arrays rebuilt when a callback view is requested.
    mutable std::vector<std::vector<record_key_view>> requested_alias_scratch_;
    mutable std::vector<retrieval_record_request_view> requested_records_scratch_;
    mutable std::vector<candidate_transfer_request_view> candidates_scratch_;
    mutable row_audit_view_storage audit_view_storage_;

    /// Estimate storage for the plan, including its temporary view arrays.
    [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
        std::size_t total = sizeof(*this);
        if (container_key) {
            total += container_key->measure_deep_size_bytes() - sizeof(*container_key);
        }
        total += requested_records.capacity() * sizeof(owning_retrieval_record_request);
        for (const auto& record_request : requested_records) {
            total += record_request.measure_deep_size_bytes() - sizeof(record_request);
        }
        total += candidates.capacity() * sizeof(owning_candidate_transfer_request);
        for (const auto& candidate : candidates) {
            total += candidate.measure_deep_size_bytes() - sizeof(candidate);
        }
        if (manifest)
            total += manifest->measure_deep_size_bytes() - sizeof(*manifest);
        total += audit.measure_deep_size_bytes() - sizeof(audit);
        total += requested_alias_scratch_.capacity() * sizeof(std::vector<record_key_view>);
        for (const auto& record_aliases : requested_alias_scratch_) {
            total += record_aliases.capacity() * sizeof(record_key_view);
        }
        total += requested_records_scratch_.capacity() * sizeof(retrieval_record_request_view);
        total += candidates_scratch_.capacity() * sizeof(candidate_transfer_request_view);
        total += audit_view_storage_.measure_storage_bytes();
        return total;
    }

    /// Build a callback view backed by this plan and its temporary arrays.
    [[nodiscard]] sequence_retrieval_plan_view view() const noexcept {
        sequence_retrieval_plan_view result;
        result.execution = execution;
        result.provider = provider;
        if (container_key)
            result.container_key = container_key->view();

        requested_alias_scratch_.resize(requested_records.size());
        requested_records_scratch_.clear();
        requested_records_scratch_.reserve(requested_records.size());
        for (std::size_t record_index = 0; record_index < requested_records.size();
             ++record_index) {
            const owning_retrieval_record_request& record_request = requested_records[record_index];
            std::vector<record_key_view>& record_aliases = requested_alias_scratch_[record_index];
            record_aliases.clear();
            record_aliases.reserve(record_request.accepted_header_aliases.size());
            for (const auto& alias : record_request.accepted_header_aliases) {
                record_aliases.push_back(alias.view());
            }
            retrieval_record_request_view record_view;
            record_view.key = record_request.key.view();
            record_view.molecule = record_request.molecule;
            record_view.origin = record_request.origin.view();
            record_view.expected_length = record_request.expected_length;
            record_view.accepted_header_aliases =
                std::span<const record_key_view>(record_aliases.data(), record_aliases.size());
            requested_records_scratch_.push_back(record_view);
        }
        result.requested_records = std::span<const retrieval_record_request_view>(
            requested_records_scratch_.data(), requested_records_scratch_.size());

        result.association = association;
        result.kind = kind;
        result.locator_policy = locator_policy;

        candidates_scratch_.clear();
        candidates_scratch_.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            candidates_scratch_.push_back(candidate.view());
        }
        result.candidates = std::span<const candidate_transfer_request_view>(
            candidates_scratch_.data(), candidates_scratch_.size());

        if (manifest)
            result.manifest = manifest->view();
        result.audit = audit_view_storage_.make_view(audit);
        return result;
    }
};

// Messages passed to provider consumers.

/// Identifies a downloaded sequence asset within one execution.
/// An asset completion message follows decoding and integrity validation.
struct sequence_asset_id {
    external_execution_key execution{}; // 256-bit execution identity
    std::uint64_t index{};              // per-execution monotonic asset ordinal

    /// Returns the fixed storage occupied by the asset identity.
    [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
        return sizeof(*this);
    }
};

/// Completion statistics and integrity result for one sequence asset.
struct owning_sequence_asset_end {
    sequence_asset_id asset_id;
    std::uint64_t record_count{};
    std::uint64_t decoded_bytes{};
    // MD5 records the provider's checksum for detecting changed data.
    owning_integrity_result integrity;

    /// Estimate storage for the completion message and its integrity result.
    [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
        std::size_t total = sizeof(*this);
        total += integrity.measure_deep_size_bytes() - sizeof(integrity);
        return total;
    }
};

/// Owning message alternatives emitted by provider sessions.
using owning_provider_message = std::variant<owning_taxonomy_row,
                                             owning_catalog_row,
                                             owning_sequence_payload,
                                             owning_sequence_retrieval_plan,
                                             owning_provider_diagnostic,
                                             owning_sequence_asset_end>;

/// Estimate the memory charged for a provider message.
[[nodiscard]] inline std::size_t
measure_deep_size_bytes(const owning_provider_message& message) noexcept {
    return std::visit([](const auto& value) noexcept { return value.measure_deep_size_bytes(); },
                      message);
}

/// A message sink takes ownership of each message and can return an error.
template <class p_sink_type>
concept provider_message_sink_compatible =
    requires(p_sink_type& sink, owning_provider_message&& message) {
        { sink.push(std::move(message)) } -> std::same_as<std::expected<void, external_error>>;
    };

/// Borrowed callback for passing an owned message to its consumer.
struct provider_message_sink_ref {
    void* object{};
    std::expected<void, external_error> (*push_fn)(void*, owning_provider_message&&){};
};

} // namespace bioseq::external::provider
