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

/// @brief Provider diagnostics, retrieval plans, and owning messages.
// The erased
// owning-message protocol. Diagnostics and the retrieval-plan VIEW types are
// transient (valid only for one sink callback).
// `owning_sequence_retrieval_plan` and `owning_provider_diagnostic` are the
// moved-through-channel owning mirrors. The four owning row/plan/diagnostic
// types form `owning_provider_message`, which sessions push through
// `provider_message_sink_ref`.
export namespace bioseq::external::provider {

// ===========================================================================
// Diagnostics (spec 1099-1123).
// ===========================================================================

enum class diagnostic_severity : std::uint8_t { warning, item_error };

struct provider_diagnostic_view {
  diagnostic_severity severity;
  provider_id provider;
  std::string_view operation;
  std::string_view item_id;
  std::string_view code;
  std::string_view message;
  bool is_retryable;
};

struct owning_provider_diagnostic {
  diagnostic_severity severity;
  provider_id provider;
  bounded_string<maximum_provider_name_bytes> provider_name;
  bounded_string<maximum_identifier_bytes> operation;
  bounded_string<maximum_identifier_bytes> item_id;
  bounded_string<256> code;
  bounded_string<maximum_diagnostic_bytes> message;
  bool is_retryable{};

  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    std::size_t total = sizeof(*this);
    total += provider_name.measure_deep_size_bytes() - sizeof(provider_name);
    total += operation.measure_deep_size_bytes() - sizeof(operation);
    total += item_id.measure_deep_size_bytes() - sizeof(item_id);
    total += code.measure_deep_size_bytes() - sizeof(code);
    total += message.measure_deep_size_bytes() - sizeof(message);
    return total;
  }

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
  bool is_replayable{};
  bool is_idempotent{};
  std::optional<std::uint64_t> expected_wire_bytes;
  std::optional<bounded_string<maximum_identifier_bytes>> etag;
  std::optional<bounded_string<maximum_identifier_bytes>> last_modified;
  std::optional<bounded_string<maximum_archive_member_name_bytes>>
      archive_member;
  std::optional<bounded_string<256>> media_type;
  std::optional<bounded_string<maximum_identifier_bytes>> compression;
  std::optional<bounded_string<maximum_identifier_bytes>> container;
  std::optional<owning_integrity_check> integrity;

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
      total +=
          last_modified->measure_deep_size_bytes() - sizeof(*last_modified);
    }
    if (archive_member) {
      total +=
          archive_member->measure_deep_size_bytes() - sizeof(*archive_member);
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

  [[nodiscard]] candidate_transfer_request_view view() const noexcept {
    candidate_transfer_request_view result;
    result.method = method;
    result.url = url.view();
    if (accept)
      result.accept = accept->view();
    if (content_type)
      result.content_type = content_type->view();
    result.immutable_body = std::span<const std::byte>(immutable_body.data(),
                                                       immutable_body.size());
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

struct owning_retrieval_manifest_rules {
  std::vector<sequence_content_kind> selected_content;
  std::optional<bounded_string<maximum_archive_member_name_bytes>>
      dataset_catalog_member;
  std::optional<bounded_string<maximum_archive_member_name_bytes>>
      fetch_manifest_member;
  std::optional<bounded_string<maximum_archive_member_name_bytes>>
      checksum_manifest_member;

  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    std::size_t total = sizeof(*this);
    total += selected_content.capacity() * sizeof(sequence_content_kind);
    if (dataset_catalog_member) {
      total += dataset_catalog_member->measure_deep_size_bytes() -
               sizeof(*dataset_catalog_member);
    }
    if (fetch_manifest_member) {
      total += fetch_manifest_member->measure_deep_size_bytes() -
               sizeof(*fetch_manifest_member);
    }
    if (checksum_manifest_member) {
      total += checksum_manifest_member->measure_deep_size_bytes() -
               sizeof(*checksum_manifest_member);
    }
    return total;
  }

  [[nodiscard]] retrieval_manifest_rules_view view() const noexcept {
    retrieval_manifest_rules_view result;
    result.selected_content = std::span<const sequence_content_kind>(
        selected_content.data(), selected_content.size());
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

  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    std::size_t total = sizeof(*this);
    total += key.measure_deep_size_bytes() - sizeof(key);
    total += origin.measure_deep_size_bytes() - sizeof(origin);
    total += accepted_header_aliases.capacity() * sizeof(owning_record_key);
    for (const auto &alias : accepted_header_aliases) {
      total += alias.measure_deep_size_bytes() - sizeof(alias);
    }
    total += audit_seed.measure_deep_size_bytes() - sizeof(audit_seed);
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

  // Transient reconstruction storage, rebuilt for each callback view.
  mutable std::vector<std::vector<record_key_view>> requested_alias_scratch_;
  mutable std::vector<retrieval_record_request_view> requested_records_scratch_;
  mutable std::vector<candidate_transfer_request_view> candidates_scratch_;
  mutable row_audit_view_storage audit_view_storage_;

  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    std::size_t total = sizeof(*this);
    if (container_key) {
      total +=
          container_key->measure_deep_size_bytes() - sizeof(*container_key);
    }
    total +=
        requested_records.capacity() * sizeof(owning_retrieval_record_request);
    for (const auto &record_request : requested_records) {
      total +=
          record_request.measure_deep_size_bytes() - sizeof(record_request);
    }
    total += candidates.capacity() * sizeof(owning_candidate_transfer_request);
    for (const auto &candidate : candidates) {
      total += candidate.measure_deep_size_bytes() - sizeof(candidate);
    }
    if (manifest)
      total += manifest->measure_deep_size_bytes() - sizeof(*manifest);
    total += audit.measure_deep_size_bytes() - sizeof(audit);
    total += requested_alias_scratch_.capacity() *
             sizeof(std::vector<record_key_view>);
    for (const auto &record_aliases : requested_alias_scratch_) {
      total += record_aliases.capacity() * sizeof(record_key_view);
    }
    total += requested_records_scratch_.capacity() *
             sizeof(retrieval_record_request_view);
    total += candidates_scratch_.capacity() *
             sizeof(candidate_transfer_request_view);
    total += audit_view_storage_.measure_storage_bytes();
    return total;
  }

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
      const owning_retrieval_record_request &record_request =
          requested_records[record_index];
      std::vector<record_key_view> &record_aliases =
          requested_alias_scratch_[record_index];
      record_aliases.clear();
      record_aliases.reserve(record_request.accepted_header_aliases.size());
      for (const auto &alias : record_request.accepted_header_aliases) {
        record_aliases.push_back(alias.view());
      }
      retrieval_record_request_view record_view;
      record_view.key = record_request.key.view();
      record_view.molecule = record_request.molecule;
      record_view.origin = record_request.origin.view();
      record_view.expected_length = record_request.expected_length;
      record_view.accepted_header_aliases = std::span<const record_key_view>(
          record_aliases.data(), record_aliases.size());
      requested_records_scratch_.push_back(record_view);
    }
    result.requested_records = std::span<const retrieval_record_request_view>(
        requested_records_scratch_.data(), requested_records_scratch_.size());

    result.association = association;
    result.kind = kind;
    result.locator_policy = locator_policy;

    candidates_scratch_.clear();
    candidates_scratch_.reserve(candidates.size());
    for (const auto &candidate : candidates) {
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

// ===========================================================================
// Erased owning-message protocol (spec 1971-1998).
// ===========================================================================

// Terminal per-asset validation/statistics event (spec §3179-3213, test #29).
// Emitted exactly once, after a sequence asset (e.g. a hydrated archive member)
// is fully decoded and its decoded-member MD5 verified; a verification FAILURE
// emits a terminal external_error instead, never this event. It produces no row
// slot — the PG access callback consumes it and continues (spec §3875-3877).
struct sequence_asset_id {
  external_execution_key execution{}; // 256-bit execution identity
  std::uint64_t index{};              // per-execution monotonic asset ordinal

  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    return sizeof(*this);
  }
};

struct owning_sequence_asset_end {
  sequence_asset_id asset_id;
  std::uint64_t record_count{};
  std::uint64_t decoded_bytes{};
  // The MD5 is recorded as provider integrity metadata, not a signature
  // (§3316).
  owning_integrity_result integrity;

  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    std::size_t total = sizeof(*this);
    total += integrity.measure_deep_size_bytes() - sizeof(integrity);
    return total;
  }
};

using owning_provider_message =
    std::variant<owning_taxonomy_row, owning_catalog_row,
                 owning_sequence_payload, owning_sequence_retrieval_plan,
                 owning_provider_diagnostic, owning_sequence_asset_end>;

// Charged-memory footprint of an owning message: visits the active alternative.
[[nodiscard]] inline std::size_t
measure_deep_size_bytes(const owning_provider_message &message) noexcept {
  return std::visit(
      [](const auto &value) noexcept {
        return value.measure_deep_size_bytes();
      },
      message);
}

template <class p_sink_type>
concept provider_message_sink_compatible =
    requires(p_sink_type &sink, owning_provider_message &&message) {
      {
        sink.push(std::move(message))
      } -> std::same_as<std::expected<void, external_error>>;
    };

struct provider_message_sink_ref {
  void *object{};
  std::expected<void, external_error> (*push_fn)(void *,
                                                 owning_provider_message &&){};
};

} // namespace bioseq::external::provider
