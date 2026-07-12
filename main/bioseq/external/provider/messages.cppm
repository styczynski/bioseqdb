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

// DOC0372 — Provider diagnostics, the retrieval-plan model, and the erased
// owning-message protocol. Diagnostics and the retrieval-plan VIEW types are
// transient (valid only for one sink callback). `owning_sequence_retrieval_plan`
// and `owning_provider_diagnostic` are the moved-through-channel owning mirrors.
// The four owning row/plan/diagnostic types form `owning_provider_message`,
// which sessions push through `provider_message_sink_ref`.
export namespace bioseq::external::provider {

// ===========================================================================
// Diagnostics (spec 1099-1123).
// ===========================================================================

enum class diagnostic_severity : std::uint8_t {
    warning,
    item_error
};

struct provider_diagnostic_view {
    diagnostic_severity severity;
    provider_id provider;
    std::string_view operation;
    std::string_view item_id;
    std::string_view code;
    std::string_view message;
    bool retryable;
};

struct owning_provider_diagnostic {
    diagnostic_severity severity;
    provider_id provider;
    bounded_string<maximum_provider_name_bytes> provider_name;
    bounded_string<maximum_identifier_bytes> operation;
    bounded_string<maximum_identifier_bytes> item_id;
    bounded_string<256> code;
    bounded_string<maximum_diagnostic_bytes> message;
    bool retryable{};

    [[nodiscard]] std::size_t deep_size_bytes() const noexcept {
        std::size_t total = sizeof(*this);
        total += provider_name.deep_size_bytes() - sizeof(provider_name);
        total += operation.deep_size_bytes() - sizeof(operation);
        total += item_id.deep_size_bytes() - sizeof(item_id);
        total += code.deep_size_bytes() - sizeof(code);
        total += message.deep_size_bytes() - sizeof(message);
        return total;
    }

    [[nodiscard]] provider_diagnostic_view view() const noexcept {
        provider_diagnostic_view out;
        out.severity = severity;
        out.provider = provider;
        out.operation = operation.view();
        out.item_id = item_id.view();
        out.code = code.view();
        out.message = message.view();
        out.retryable = retryable;
        return out;
    }
};

// ===========================================================================
// Retrieval plan model — transient VIEW types (spec 803-919).
// (locator_policy_id lives in bioseq.external.provider.base.)
// ===========================================================================

enum class transfer_method : std::uint8_t { get, post };

enum class locator_scheme : std::uint8_t { https, http, ftps, ftp };

struct candidate_transfer_request_view {
    transfer_method method;
    std::string_view url;
    std::optional<std::string_view> accept;
    std::optional<std::string_view> content_type;
    std::span<const std::byte> immutable_body;
    bool replayable;
    bool idempotent;
    std::optional<std::uint64_t> expected_wire_bytes;
    std::optional<std::string_view> etag;
    std::optional<std::string_view> last_modified;
    std::optional<std::string_view> archive_member;
    std::optional<std::string_view> media_type;
    std::optional<std::string_view> compression;
    std::optional<std::string_view> container;
    std::optional<integrity_check_view> integrity;
};

enum class retrieval_plan_kind : std::uint8_t {
    direct_asset,
    archive_manifest,
    dehydrated_manifest,
    bounded_endpoint,
    bulk_export,
    batch_endpoint
};

enum class sequence_content_kind : std::uint8_t {
    genomic_fasta,
    transcript_fasta,
    cds_fasta,
    protein_fasta
};

struct retrieval_manifest_rules_view {
    std::span<const sequence_content_kind> selected_content;
    std::optional<std::string_view> dataset_catalog_member;
    std::optional<std::string_view> fetch_manifest_member;
    std::optional<std::string_view> checksum_manifest_member;
};

struct retrieval_record_request_view {
    record_key_view key;
    molecule_kind molecule;
    sequence_origin_view origin;
    std::optional<std::uint64_t> expected_length;
    std::span<const record_key_view> accepted_header_aliases;
};

enum class record_association_rule : std::uint8_t {
    exact_single,
    fasta_header_accession,
    manifest_catalog
};

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

// ===========================================================================
// Retrieval plan model — OWNING mirrors.
// ===========================================================================

// Owning mirror of candidate_transfer_request_view. The immutable replay body
// is owned as raw bytes (spec 931-936); credentials/secret headers are never
// serialized into a plan.
struct owning_candidate_transfer_request {
    transfer_method method{};
    bounded_string<maximum_url_bytes> url;
    std::optional<bounded_string<maximum_identifier_bytes>> accept;
    std::optional<bounded_string<maximum_identifier_bytes>> content_type;
    std::vector<std::byte> immutable_body;
    bool replayable{};
    bool idempotent{};
    std::optional<std::uint64_t> expected_wire_bytes;
    std::optional<bounded_string<maximum_identifier_bytes>> etag;
    std::optional<bounded_string<maximum_identifier_bytes>> last_modified;
    std::optional<bounded_string<maximum_archive_member_name_bytes>>
        archive_member;
    std::optional<bounded_string<256>> media_type;
    std::optional<bounded_string<maximum_identifier_bytes>> compression;
    std::optional<bounded_string<maximum_identifier_bytes>> container;
    std::optional<owning_integrity_check> integrity;

    [[nodiscard]] std::size_t deep_size_bytes() const noexcept {
        std::size_t total = sizeof(*this);
        total += url.deep_size_bytes() - sizeof(url);
        if (accept) total += accept->deep_size_bytes() - sizeof(*accept);
        if (content_type) {
            total += content_type->deep_size_bytes() - sizeof(*content_type);
        }
        total += immutable_body.capacity() * sizeof(std::byte);
        if (etag) total += etag->deep_size_bytes() - sizeof(*etag);
        if (last_modified) {
            total += last_modified->deep_size_bytes() - sizeof(*last_modified);
        }
        if (archive_member) {
            total += archive_member->deep_size_bytes() - sizeof(*archive_member);
        }
        if (media_type) {
            total += media_type->deep_size_bytes() - sizeof(*media_type);
        }
        if (compression) {
            total += compression->deep_size_bytes() - sizeof(*compression);
        }
        if (container) {
            total += container->deep_size_bytes() - sizeof(*container);
        }
        if (integrity) {
            total += integrity->deep_size_bytes() - sizeof(*integrity);
        }
        return total;
    }

    [[nodiscard]] candidate_transfer_request_view view() const noexcept {
        candidate_transfer_request_view out;
        out.method = method;
        out.url = url.view();
        if (accept) out.accept = accept->view();
        if (content_type) out.content_type = content_type->view();
        out.immutable_body = std::span<const std::byte>(immutable_body.data(),
                                                        immutable_body.size());
        out.replayable = replayable;
        out.idempotent = idempotent;
        out.expected_wire_bytes = expected_wire_bytes;
        if (etag) out.etag = etag->view();
        if (last_modified) out.last_modified = last_modified->view();
        if (archive_member) out.archive_member = archive_member->view();
        if (media_type) out.media_type = media_type->view();
        if (compression) out.compression = compression->view();
        if (container) out.container = container->view();
        if (integrity) out.integrity = integrity->view();
        return out;
    }
};

struct owning_retrieval_manifest_rules {
    std::vector<sequence_content_kind> selected_content;
    std::optional<bounded_string<maximum_archive_member_name_bytes>>
        dataset_catalog_member;
    std::optional<bounded_string<maximum_archive_member_name_bytes>>
        fetch_manifest_member;
    std::optional<bounded_string<maximum_archive_member_name_bytes>>
        checksum_manifest_member;

    [[nodiscard]] std::size_t deep_size_bytes() const noexcept {
        std::size_t total = sizeof(*this);
        total += selected_content.capacity() * sizeof(sequence_content_kind);
        if (dataset_catalog_member) {
            total += dataset_catalog_member->deep_size_bytes() -
                     sizeof(*dataset_catalog_member);
        }
        if (fetch_manifest_member) {
            total += fetch_manifest_member->deep_size_bytes() -
                     sizeof(*fetch_manifest_member);
        }
        if (checksum_manifest_member) {
            total += checksum_manifest_member->deep_size_bytes() -
                     sizeof(*checksum_manifest_member);
        }
        return total;
    }

    [[nodiscard]] retrieval_manifest_rules_view view() const noexcept {
        retrieval_manifest_rules_view out;
        out.selected_content = std::span<const sequence_content_kind>(
            selected_content.data(), selected_content.size());
        if (dataset_catalog_member) {
            out.dataset_catalog_member = dataset_catalog_member->view();
        }
        if (fetch_manifest_member) {
            out.fetch_manifest_member = fetch_manifest_member->view();
        }
        if (checksum_manifest_member) {
            out.checksum_manifest_member = checksum_manifest_member->view();
        }
        return out;
    }
};

// Spec 2151-2158. Transcribed exactly; the corresponding view
// (retrieval_record_request_view) carries no audit, so no view() is exposed
// here — the owning plan rebuilds record views with its own alias scratch.
struct owning_retrieval_record_request {
    owning_record_key key;
    molecule_kind molecule{};
    owning_sequence_origin origin;
    std::optional<std::uint64_t> expected_length;
    std::vector<owning_record_key> accepted_header_aliases;
    owning_sequence_audit_seed audit_seed;

    [[nodiscard]] std::size_t deep_size_bytes() const noexcept {
        std::size_t total = sizeof(*this);
        total += key.deep_size_bytes() - sizeof(key);
        total += origin.deep_size_bytes() - sizeof(origin);
        total += accepted_header_aliases.capacity() * sizeof(owning_record_key);
        for (const auto& alias : accepted_header_aliases) {
            total += alias.deep_size_bytes() - sizeof(alias);
        }
        total += audit_seed.deep_size_bytes() - sizeof(audit_seed);
        return total;
    }
};

// Owning mirror of sequence_retrieval_plan_view. Nested per-record alias spans
// and the candidate/record view arrays are rebuilt into mutable scratch owned
// by the plan; the returned view outlives only the callback.
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

    // Transient reconstruction scratch (mutable; rebuilt by view()).
    mutable std::vector<std::vector<record_key_view>> requested_alias_scratch_;
    mutable std::vector<retrieval_record_request_view> requested_records_scratch_;
    mutable std::vector<candidate_transfer_request_view> candidates_scratch_;
    mutable std::vector<provenance_hop_view> audit_provenance_scratch_;

    [[nodiscard]] std::size_t deep_size_bytes() const noexcept {
        std::size_t total = sizeof(*this);
        if (container_key) {
            total += container_key->deep_size_bytes() - sizeof(*container_key);
        }
        total += requested_records.capacity() *
                 sizeof(owning_retrieval_record_request);
        for (const auto& req : requested_records) {
            total += req.deep_size_bytes() - sizeof(req);
        }
        total += candidates.capacity() *
                 sizeof(owning_candidate_transfer_request);
        for (const auto& cand : candidates) {
            total += cand.deep_size_bytes() - sizeof(cand);
        }
        if (manifest) total += manifest->deep_size_bytes() - sizeof(*manifest);
        total += audit.deep_size_bytes() - sizeof(audit);
        total += requested_alias_scratch_.capacity() *
                 sizeof(std::vector<record_key_view>);
        for (const auto& inner : requested_alias_scratch_) {
            total += inner.capacity() * sizeof(record_key_view);
        }
        total += requested_records_scratch_.capacity() *
                 sizeof(retrieval_record_request_view);
        total += candidates_scratch_.capacity() *
                 sizeof(candidate_transfer_request_view);
        total += audit_provenance_scratch_.capacity() *
                 sizeof(provenance_hop_view);
        return total;
    }

    [[nodiscard]] sequence_retrieval_plan_view view() const noexcept {
        sequence_retrieval_plan_view out;
        out.execution = execution;
        out.provider = provider;
        if (container_key) out.container_key = container_key->view();

        requested_alias_scratch_.resize(requested_records.size());
        requested_records_scratch_.clear();
        requested_records_scratch_.reserve(requested_records.size());
        for (std::size_t i = 0; i < requested_records.size(); ++i) {
            const owning_retrieval_record_request& req = requested_records[i];
            std::vector<record_key_view>& alias_buf = requested_alias_scratch_[i];
            alias_buf.clear();
            alias_buf.reserve(req.accepted_header_aliases.size());
            for (const auto& alias : req.accepted_header_aliases) {
                alias_buf.push_back(alias.view());
            }
            retrieval_record_request_view rv;
            rv.key = req.key.view();
            rv.molecule = req.molecule;
            rv.origin = req.origin.view();
            rv.expected_length = req.expected_length;
            rv.accepted_header_aliases = std::span<const record_key_view>(
                alias_buf.data(), alias_buf.size());
            requested_records_scratch_.push_back(rv);
        }
        out.requested_records = std::span<const retrieval_record_request_view>(
            requested_records_scratch_.data(),
            requested_records_scratch_.size());

        out.association = association;
        out.kind = kind;
        out.locator_policy = locator_policy;

        candidates_scratch_.clear();
        candidates_scratch_.reserve(candidates.size());
        for (const auto& cand : candidates) {
            candidates_scratch_.push_back(cand.view());
        }
        out.candidates = std::span<const candidate_transfer_request_view>(
            candidates_scratch_.data(), candidates_scratch_.size());

        if (manifest) out.manifest = manifest->view();
        out.audit = to_row_audit_view(audit, audit_provenance_scratch_);
        return out;
    }
};

// ===========================================================================
// Erased owning-message protocol (spec 1971-1998).
// ===========================================================================

using owning_provider_message = std::variant<
    owning_taxonomy_row,
    owning_catalog_row,
    owning_sequence_payload,
    owning_sequence_retrieval_plan,
    owning_provider_diagnostic>;

// Charged-memory footprint of an owning message: visits the active alternative.
[[nodiscard]] inline std::size_t deep_size_bytes(
    const owning_provider_message& message) noexcept {
    return std::visit(
        [](const auto& value) noexcept { return value.deep_size_bytes(); },
        message);
}

template <class S>
concept is_provider_message_sink =
    requires(S& sink, owning_provider_message&& message) {
        { sink.push(std::move(message)) }
            -> std::same_as<std::expected<void, external_error>>;
    };

struct provider_message_sink_ref {
    void* object{};
    std::expected<void, external_error> (*push_fn)(
        void*, owning_provider_message&&){};
};

}  // namespace bioseq::external::provider
