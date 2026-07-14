#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import bioseq.cache.core;

using namespace bioseq::cache;

namespace {

[[nodiscard]] std::span<const std::byte>
byte_view(std::string_view value) noexcept {
  return {
      reinterpret_cast<const std::byte *>(value.data()),
      value.size(),
  };
}

[[nodiscard]] std::span<const char>
char_view(std::string_view value) noexcept {
  return {value.data(), value.size()};
}

[[nodiscard]] CacheDigest256 digest(std::uint8_t seed) noexcept {
  CacheDigest256 result{};
  for (std::size_t index = 0; index < result.bytes.size(); ++index) {
    result.bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return result;
}

[[nodiscard]] ReusableCoverageDirectoryKeyV1
coverage_key(std::uint32_t role, std::uint8_t domain_seed = 90) noexcept {
  return ReusableCoverageDirectoryKeyV1{
      .security_namespace =
          {
              .database_oid = 11,
              .current_role_oid = role,
              .session_user_oid = role,
              .security_context_flags = 3,
              .row_security_mode = 1,
              .provider_security_digest =
                  digest(static_cast<std::uint8_t>(role)),
          },
      .coverage_domain_digest = digest(domain_seed),
  };
}

[[nodiscard]] ReusableCoverageDirectoryKeyV1 coverage_key(
    std::uint32_t role, std::uint8_t domain_seed,
    std::uint32_t database_oid) noexcept {
  ReusableCoverageDirectoryKeyV1 result = coverage_key(role, domain_seed);
  result.security_namespace.database_oid = database_oid;
  return result;
}

[[nodiscard]] ReusableGenerationMetadataV1
metadata(std::uint64_t producer_ttl_ms = 60'000) noexcept {
  return ReusableGenerationMetadataV1{
      .provider_id = 7,
      .provider_abi_version = 2,
      .provider_domain_abi_version = 3,
      .dependency_count = 2,
      .compatibility_family_id = 41,
      .capability_mask = 0b1111,
      .schema_fingerprint = digest(101),
      .mapping_fingerprint = digest(102),
      .lineage_fingerprint = digest(103),
      .dependency_fingerprint = digest(104),
      .observability = WriteObservability::ProviderComplete,
      .estimated_covered_rows = 50,
      .producer_ttl_ms = producer_ttl_ms,
  };
}

[[nodiscard]] ReusableProducerIdentityV1
producer(std::uint32_t ordinal) noexcept {
  return ReusableProducerIdentityV1{
      .proc_number = ordinal,
      .pid = static_cast<std::int32_t>(10'000 + ordinal),
      .backend_nonce = 20'000 + ordinal,
      .backend_started_at_us = 30'000 + ordinal,
  };
}

[[nodiscard]] PhysicalIdentityV1
physical_identity(std::uint8_t seed) noexcept {
  PhysicalIdentityV1 result{
      .kind = PhysicalIdentityKind::ProviderOpaque,
      .length = 4,
  };
  for (std::size_t index = 0; index < result.length; ++index) {
    result.bytes[index] = std::byte{static_cast<std::uint8_t>(seed + index)};
  }
  return result;
}

[[nodiscard]] ReusableReferenceOwnerV1
reference_owner(std::uint32_t ordinal) noexcept {
  return producer(1'000 + ordinal);
}

template <class Row>
concept exposes_reusable_shared_byte_ranges = requires(Row row) {
  row.canonical_generation_key;
  row.canonical_domain_key;
  row.coverage_descriptor;
};

static_assert(!exposes_reusable_shared_byte_ranges<
              ReusableGenerationDiagnosticsRowV1>);

template <class Row>
concept exposes_reusable_diagnostic_text = requires(Row row) {
  row.display_sql;
  row.producer_plan;
};

static_assert(!exposes_reusable_diagnostic_text<
              ReusableGenerationDiagnosticsRowV1>);

struct DiagnosticsPartitionTrace {
  std::uint32_t exact_acquires = 0;
  std::uint32_t exact_releases = 0;
  std::uint32_t coverage_acquires = 0;
  std::uint32_t coverage_releases = 0;
};

void trace_exact_acquire(void *opaque, std::uint32_t) noexcept {
  auto &trace = *static_cast<DiagnosticsPartitionTrace *>(opaque);
  ++trace.exact_acquires;
}

void trace_exact_release(void *opaque, std::uint32_t) noexcept {
  auto &trace = *static_cast<DiagnosticsPartitionTrace *>(opaque);
  ++trace.exact_releases;
}

void trace_coverage_acquire(void *opaque, std::uint32_t) noexcept {
  auto &trace = *static_cast<DiagnosticsPartitionTrace *>(opaque);
  ++trace.coverage_acquires;
}

void trace_coverage_release(void *opaque, std::uint32_t) noexcept {
  auto &trace = *static_cast<DiagnosticsPartitionTrace *>(opaque);
  ++trace.coverage_releases;
}

[[nodiscard]] const ReusableGenerationDiagnosticsRowV1 *diagnostics_row(
    std::span<const ReusableGenerationDiagnosticsRowV1> rows,
    const ReusableGenerationReferenceV1 &reference) noexcept {
  for (const ReusableGenerationDiagnosticsRowV1 &row : rows) {
    if (row.reference == reference) {
      return &row;
    }
  }
  return nullptr;
}

class RegistryFixture {
private:
  ReusableRegistryConfigV1 config_{};
  std::vector<std::uint64_t> region_{};

public:
  explicit RegistryFixture(std::uint32_t max_generations,
                           std::uint32_t arena_bytes = 1U << 20,
                           std::uint32_t partitions = 4)
      : config_{
            .max_generations = max_generations,
            .partition_count = partitions,
            .byte_arena_bytes = arena_bytes,
        } {
    const auto request = reusable_registry_preload_request(config_);
    if (!request.has_value()) {
      throw std::runtime_error("invalid registry test configuration");
    }
    request_ = *request;
    region_.resize(request_.layout.total_bytes / sizeof(std::uint64_t));
    std::span<std::byte> bytes{
        reinterpret_cast<std::byte *>(region_.data()),
        request_.layout.total_bytes,
    };
    auto started = startup_reusable_registry(bytes, kBootNonce, config_);
    if (!started.has_value()) {
      throw std::runtime_error("registry startup failed");
    }
    attachment = *started;
  }

  static constexpr std::uint64_t kBootNonce = 0xB105E0ULL;
  ReusableRegistryAttachmentV1 attachment{};
  ReusableRegistryPreloadRequestV1 request_{};
};

[[nodiscard]] ReusableClaimResultV1
claim_with_metadata(
    RegistryFixture &fixture, const ReusableCoverageDirectoryKeyV1 &key,
    const CacheDigest256 &generation_digest, std::string_view domain,
    std::string_view generation, std::uint32_t producer_ordinal,
    const ReusableGenerationMetadataV1 &generation_metadata,
    std::uint64_t started_ns = 100,
    std::string_view coverage_descriptor = {}) noexcept {
  return claim_reusable_generation(
      fixture.attachment, ReusableClaimRequestV1{
                              .coverage_directory_key = key,
                              .generation_digest = generation_digest,
                              .canonical_domain_key = byte_view(domain),
                              .canonical_generation_key = byte_view(generation),
                              .coverage_descriptor =
                                  byte_view(coverage_descriptor),
                              .metadata = generation_metadata,
                              .producer = producer(producer_ordinal),
                              .reference_owner =
                                  reference_owner(producer_ordinal),
                              .started_monotonic_ns = started_ns,
                          });
}

[[nodiscard]] ReusableClaimResultV1
claim(RegistryFixture &fixture, const ReusableCoverageDirectoryKeyV1 &key,
      const CacheDigest256 &generation_digest, std::string_view domain,
      std::string_view generation, std::uint32_t producer_ordinal,
      std::uint64_t started_ns = 100,
      std::uint64_t producer_ttl_ms = 60'000,
      std::string_view coverage_descriptor = {}) noexcept {
  return claim_with_metadata(fixture, key, generation_digest, domain,
                             generation, producer_ordinal,
                             metadata(producer_ttl_ms), started_ns,
                             coverage_descriptor);
}

[[nodiscard]] ReusableManifestHandleV1
publish(RegistryFixture &fixture,
        const ReusableGenerationReferenceV1 &reference,
        std::uint64_t completed_ns = 200) noexcept {
  if (begin_reusable_build(fixture.attachment, reference) !=
      ReusableRegistryErrorV1::None) {
    return {};
  }
  const ReusableManifestHandleV1 handle{
      .slot = reference.generation_slot,
      .slot_sequence = reference.slot_sequence + 100,
      .id = reference.id,
  };
  if (publish_reusable_generation(fixture.attachment,
                                  ReusablePublishRequestV1{
                                      .reference = reference,
                                      .manifest = handle,
                                      .completed_monotonic_ns = completed_ns,
                                      .row_count = 17,
                                      .logical_bytes = 700,
                                      .stored_bytes = 300,
                                  }) != ReusableRegistryErrorV1::None) {
    return {};
  }
  return handle;
}

[[nodiscard]] ReusableCandidateRequestV1
candidate_request(const ReusableCoverageDirectoryKeyV1 &key,
                  std::string_view domain) noexcept {
  return ReusableCandidateRequestV1{
      .coverage_directory_key = key,
      .canonical_domain_key = byte_view(domain),
      .reference_owner =
          reference_owner(key.security_namespace.current_role_oid),
      .provider_id = 7,
      .compatibility_family_id = 41,
      .required_capability_mask = 0b0101,
  };
}

[[nodiscard]] bool
producer_is_dead(void *, const ReusableProducerIdentityV1 &) noexcept {
  return false;
}

[[nodiscard]] bool
producer_is_alive(void *, const ReusableProducerIdentityV1 &) noexcept {
  return true;
}

[[nodiscard]] bool
reference_owner_is_dead(void *, const ReusableReferenceOwnerV1 &) noexcept {
  return false;
}

} // namespace

TEST_CASE("reusable registry sizes exactly and coalesces only full keys") {
  RegistryFixture fixture{8};
  REQUIRE(fixture.request_.membership_lock_count == 1);
  REQUIRE(fixture.request_.exact_partition_lock_count == 4);
  REQUIRE(fixture.request_.coverage_partition_lock_count == 4);
  REQUIRE(fixture.attachment.region.size() ==
          fixture.request_.layout.total_bytes);
  REQUIRE(validate_reusable_registry(fixture.attachment) ==
          ReusableRegistryErrorV1::None);

  const auto key = coverage_key(22);
  const CacheDigest256 forced_collision = digest(1);
  auto first =
      claim(fixture, key, forced_collision, "domain-a", "generation-a", 1);
  REQUIRE(first.disposition == ReusableClaimDispositionV1::ClaimedProducer);

  auto same =
      claim(fixture, key, forced_collision, "domain-a", "generation-a", 2);
  REQUIRE(same.disposition == ReusableClaimDispositionV1::ExistingInFlight);
  REQUIRE(same.reference == first.reference);
  REQUIRE(same.token.active);
  REQUIRE(same.token.kind == ReusableReferenceKindV1::Waiter);

  auto digest_collision =
      claim(fixture, key, forced_collision, "domain-a", "generation-b", 3);
  REQUIRE(digest_collision.disposition ==
          ReusableClaimDispositionV1::ClaimedProducer);
  REQUIRE(digest_collision.reference != first.reference);
  REQUIRE(release_reusable_reference(fixture.attachment, same.token) ==
          ReusableRegistryErrorV1::None);

  const auto handle = publish(fixture, first.reference);
  REQUIRE(static_cast<bool>(handle));
  auto lookup = lookup_reusable_current(
      fixture.attachment, key, byte_view("domain-a"), forced_collision,
      byte_view("generation-a"), ReusableLookupModeV1::PinComplete,
      reference_owner(90));
  REQUIRE(lookup.error == ReusableRegistryErrorV1::None);
  REQUIRE(lookup.snapshot.state == CacheEntryState::Complete);
  REQUIRE(lookup.snapshot.manifest.id == first.reference.id);
  REQUIRE(lookup.token.kind == ReusableReferenceKindV1::ReaderPin);
  REQUIRE(release_reusable_reference(fixture.attachment, lookup.token) ==
          ReusableRegistryErrorV1::None);
  REQUIRE(validate_reusable_registry(fixture.attachment) ==
          ReusableRegistryErrorV1::None);
}

TEST_CASE("coverage lookup proves full domain bytes after digest match") {
  RegistryFixture fixture{8};
  const auto key = coverage_key(23, 70);
  auto left = claim(fixture, key, digest(10), "domain-left", "left-v1", 1);
  auto right = claim(fixture, key, digest(11), "domain-right", "right-v1", 2);
  REQUIRE(left);
  REQUIRE(right);

  auto candidates = snapshot_reusable_candidates(
      fixture.attachment, candidate_request(key, "domain-left"));
  REQUIRE(candidates.has_value());
  REQUIRE(candidates->visited_count == 2);
  REQUIRE(candidates->domain_collision_count == 1);
  REQUIRE(candidates->candidate_count == 1);
  REQUIRE(candidates->candidates[0].generation.reference == left.reference);
  REQUIRE(release_reusable_candidate_batch(fixture.attachment, *candidates) ==
          ReusableRegistryErrorV1::None);
  REQUIRE(validate_reusable_registry(fixture.attachment) ==
          ReusableRegistryErrorV1::None);
}

TEST_CASE("security namespaces own independent bounded 64 member domains") {
  RegistryFixture fixture{66, 1U << 20, 1};
  const auto crowded_key = coverage_key(30, 80);
  std::vector<ReusableGenerationReferenceV1> references;
  references.reserve(kMaxCoverageGenerationsPerDomain);
  for (std::uint32_t index = 0; index < kMaxCoverageGenerationsPerDomain;
       ++index) {
    const std::string generation = "generation-" + std::to_string(index);
    auto claimed =
        claim(fixture, crowded_key, digest(static_cast<std::uint8_t>(index)),
              "crowded-domain", generation, index + 1U);
    REQUIRE(claimed.disposition == ReusableClaimDispositionV1::ClaimedProducer);
    references.push_back(claimed.reference);
  }

  auto overflow = claim(fixture, crowded_key, digest(200), "crowded-domain",
                        "overflow", 100);
  REQUIRE(overflow.error == ReusableRegistryErrorV1::CoverageBucketFull);

  const auto other_namespace = coverage_key(31, 80);
  auto independent = claim(fixture, other_namespace, digest(201),
                           "crowded-domain", "independent", 101);
  REQUIRE(independent.disposition ==
          ReusableClaimDispositionV1::ClaimedProducer);

  auto candidates = snapshot_reusable_candidates(
      fixture.attachment, candidate_request(crowded_key, "crowded-domain"));
  REQUIRE(candidates.has_value());
  REQUIRE(candidates->candidate_count == kMaxCoverageGenerationsPerDomain);
  REQUIRE(candidates->selection_truncated);
  for (std::size_t index = 1; index < candidates->candidate_count; ++index) {
    REQUIRE(
        candidates->candidates[index - 1U]
            .generation.reference.id.claim_sequence <
        candidates->candidates[index].generation.reference.id.claim_sequence);
  }
  REQUIRE(release_reusable_candidate_batch(fixture.attachment, *candidates) ==
          ReusableRegistryErrorV1::None);

  auto isolated = snapshot_reusable_candidates(
      fixture.attachment, candidate_request(other_namespace, "crowded-domain"));
  REQUIRE(isolated.has_value());
  REQUIRE(isolated->visited_count == 1);
  REQUIRE(isolated->candidate_count == 1);
  REQUIRE(isolated->candidates[0].generation.reference ==
          independent.reference);
  REQUIRE(release_reusable_candidate_batch(fixture.attachment, *isolated) ==
          ReusableRegistryErrorV1::None);
  REQUIRE(validate_reusable_registry(fixture.attachment) ==
          ReusableRegistryErrorV1::None);
}

TEST_CASE("every reusable security field partitions discovery") {
  RegistryFixture fixture{8, 1U << 20, 1};
  const auto base = coverage_key(33, 81);
  std::array<ReusableCoverageDirectoryKeyV1, 7> keys{};
  keys.fill(base);
  ++keys[1].security_namespace.database_oid;
  ++keys[2].security_namespace.current_role_oid;
  ++keys[3].security_namespace.session_user_oid;
  ++keys[4].security_namespace.security_context_flags;
  keys[5].security_namespace.row_security_mode = 0;
  keys[6].security_namespace.provider_security_digest =
      kProviderIndependentSecurityDigestV1;

  std::array<ReusableGenerationReferenceV1, 7> references{};
  for (std::size_t index = 0; index < keys.size(); ++index) {
    const std::string generation = "security-generation-" +
                                   std::to_string(index);
    auto claimed = claim(
        fixture, keys[index], digest(static_cast<std::uint8_t>(120 + index)),
        "one-domain", generation, static_cast<std::uint32_t>(index + 1));
    REQUIRE(claimed.disposition ==
            ReusableClaimDispositionV1::ClaimedProducer);
    references[index] = claimed.reference;
  }

  for (std::size_t index = 0; index < keys.size(); ++index) {
    auto candidates = snapshot_reusable_candidates(
        fixture.attachment, candidate_request(keys[index], "one-domain"));
    REQUIRE(candidates.has_value());
    REQUIRE(candidates->visited_count == 1);
    REQUIRE(candidates->candidate_count == 1);
    REQUIRE(candidates->candidates[0].generation.reference ==
            references[index]);
    REQUIRE(release_reusable_candidate_batch(
                fixture.attachment, *candidates) ==
            ReusableRegistryErrorV1::None);
  }
  REQUIRE(validate_reusable_registry(fixture.attachment) ==
          ReusableRegistryErrorV1::None);
}

TEST_CASE("reusable security namespace rejects invalid sentinels") {
  RegistryFixture fixture{2};
  const auto valid = coverage_key(34, 82);
  auto require_rejected = [&fixture](ReusableCoverageDirectoryKeyV1 key) {
    const auto result = claim(
        fixture, key, digest(140), "domain", "generation", 1);
    REQUIRE(result.error == ReusableRegistryErrorV1::InvalidArgument);
    REQUIRE(result.disposition == ReusableClaimDispositionV1::Rejected);
  };

  auto invalid = valid;
  invalid.security_namespace.database_oid = 0;
  require_rejected(invalid);
  invalid = valid;
  invalid.security_namespace.current_role_oid = 0;
  require_rejected(invalid);
  invalid = valid;
  invalid.security_namespace.session_user_oid = 0;
  require_rejected(invalid);
  invalid = valid;
  invalid.security_namespace.row_security_mode = -1;
  require_rejected(invalid);
  invalid.security_namespace.row_security_mode = 2;
  require_rejected(invalid);
  invalid = valid;
  invalid.security_namespace.reserved = 1;
  require_rejected(invalid);

  auto provider_independent = valid;
  provider_independent.security_namespace.provider_security_digest =
      kProviderIndependentSecurityDigestV1;
  REQUIRE(claim(
              fixture, provider_independent, digest(141), "domain",
              "provider-independent", 2)
              .disposition ==
          ReusableClaimDispositionV1::ClaimedProducer);
}

TEST_CASE("two index retirement and reclaim reject stale ABA references") {
  RegistryFixture fixture{1, 4U << 10, 1};
  const auto key = coverage_key(40);
  auto claimed = claim(fixture, key, digest(40), "domain", "generation", 1);
  REQUIRE(claimed);
  const auto old_reference = claimed.reference;
  const auto manifest = publish(fixture, old_reference);
  REQUIRE(static_cast<bool>(manifest));

  auto planner_pin = pin_reusable_generation(
      fixture.attachment, old_reference, ReusableReferenceKindV1::PlannerPin,
      reference_owner(91));
  REQUIRE(planner_pin.has_value());
  auto pinned_retire =
      retire_reusable_generation(fixture.attachment, old_reference);
  REQUIRE_FALSE(pinned_retire.has_value());
  REQUIRE(pinned_retire.error() == ReusableRegistryErrorV1::InvalidState);
  REQUIRE(release_reusable_reference(fixture.attachment, *planner_pin) ==
          ReusableRegistryErrorV1::None);

  auto retired = retire_reusable_generation(fixture.attachment, old_reference);
  REQUIRE(retired.has_value());
  REQUIRE(retired->id == manifest.id);
  REQUIRE(validate_reusable_registry(fixture.attachment) ==
          ReusableRegistryErrorV1::None);

  auto exact = lookup_reusable_current(
      fixture.attachment, key, byte_view("domain"), digest(40),
      byte_view("generation"), ReusableLookupModeV1::Observe,
      reference_owner(92));
  REQUIRE(exact.error == ReusableRegistryErrorV1::NotFound);
  auto coverage = snapshot_reusable_candidates(
      fixture.attachment, candidate_request(key, "domain"));
  REQUIRE(coverage.has_value());
  REQUIRE(coverage->candidate_count == 0);

  auto reclaimed = reclaim_retired_reusable_generations(fixture.attachment, 1);
  REQUIRE(reclaimed.has_value());
  REQUIRE(*reclaimed == 1);
  auto reused = claim(fixture, key, digest(40), "domain", "generation", 2);
  REQUIRE(reused);
  REQUIRE(reused.reference.generation_slot == old_reference.generation_slot);
  REQUIRE(reused.reference.slot_sequence != old_reference.slot_sequence);
  auto stale = snapshot_reusable_generation(fixture.attachment, old_reference);
  REQUIRE_FALSE(stale.has_value());
  REQUIRE(stale.error() == ReusableRegistryErrorV1::StaleReference);
  REQUIRE(validate_reusable_registry(fixture.attachment) ==
          ReusableRegistryErrorV1::None);
}

TEST_CASE("reusable reference owners are ABA safe and bounded") {
  RegistryFixture fixture{2, 16U << 10, 1};
  const auto key = coverage_key(45);
  auto claimed = claim(fixture, key, digest(45), "domain", "generation", 1);
  REQUIRE(claimed);

  std::vector<ReusableReferenceTokenV1> pins;
  pins.reserve(kReusableReferenceOwnersPerGenerationV1);
  for (std::uint32_t index = 0;
       index < kReusableReferenceOwnersPerGenerationV1; ++index) {
    auto pin = pin_reusable_generation(
        fixture.attachment, claimed.reference,
        ReusableReferenceKindV1::PlannerPin, reference_owner(200 + index));
    REQUIRE(pin.has_value());
    pins.push_back(*pin);
  }
  auto overflow = pin_reusable_generation(
      fixture.attachment, claimed.reference,
      ReusableReferenceKindV1::PlannerPin, reference_owner(300));
  REQUIRE_FALSE(overflow.has_value());
  REQUIRE(overflow.error() == ReusableRegistryErrorV1::ReferenceSlotsFull);

  for (ReusableReferenceTokenV1 &pin : pins) {
    REQUIRE(release_reusable_reference(fixture.attachment, pin) ==
            ReusableRegistryErrorV1::None);
  }
  REQUIRE(validate_reusable_registry(fixture.attachment) ==
          ReusableRegistryErrorV1::None);
}

TEST_CASE("janitor reaps exact dead waiter planner and reader owners") {
  RegistryFixture fixture{4, 16U << 10, 1};
  const auto key = coverage_key(46);
  auto claimed = claim(fixture, key, digest(46), "domain", "generation", 1);
  REQUIRE(claimed);
  auto converting =
      claim(fixture, key, digest(46), "domain", "generation", 2);
  auto dead_waiter =
      claim(fixture, key, digest(46), "domain", "generation", 3);
  REQUIRE(converting.token.kind == ReusableReferenceKindV1::Waiter);
  REQUIRE(dead_waiter.token.kind == ReusableReferenceKindV1::Waiter);

  auto dead_planner = pin_reusable_generation(
      fixture.attachment, claimed.reference,
      ReusableReferenceKindV1::PlannerPin, reference_owner(400));
  REQUIRE(dead_planner.has_value());
  REQUIRE(static_cast<bool>(publish(fixture, claimed.reference)));

  const std::uint32_t converting_slot = converting.token.owner_slot;
  const std::uint64_t converting_sequence = converting.token.owner_sequence;
  REQUIRE(convert_reusable_waiter_to_reader(fixture.attachment,
                                            converting.token) ==
          ReusableRegistryErrorV1::None);
  REQUIRE(converting.token.kind == ReusableReferenceKindV1::ReaderPin);
  REQUIRE(converting.token.owner_slot == converting_slot);
  REQUIRE(converting.token.owner_sequence == converting_sequence);
  REQUIRE(release_reusable_reference(fixture.attachment, converting.token) ==
          ReusableRegistryErrorV1::None);

  auto dead_reader = lookup_reusable_current(
      fixture.attachment, key, byte_view("domain"), digest(46),
      byte_view("generation"), ReusableLookupModeV1::PinComplete,
      reference_owner(401));
  REQUIRE(dead_reader.error == ReusableRegistryErrorV1::None);
  auto before = snapshot_reusable_generation(fixture.attachment,
                                             claimed.reference);
  REQUIRE(before.has_value());
  REQUIRE(before->waiter_count == 1);
  REQUIRE(before->planner_pin_count == 1);
  REQUIRE(before->reader_pin_count == 1);

  const auto sweep = janitor_reusable_registry(
      fixture.attachment, 1'000, kMaximumReusableJanitorBatchV1,
      ReusableJanitorCallbacksV1{
          .producer_alive = producer_is_alive,
          .reference_owner_alive = reference_owner_is_dead,
      });
  REQUIRE(sweep.error == ReusableRegistryErrorV1::None);
  REQUIRE(sweep.reaped_waiters == 1);
  REQUIRE(sweep.reaped_planner_pins == 1);
  REQUIRE(sweep.reaped_reader_pins == 1);
  auto after = snapshot_reusable_generation(fixture.attachment,
                                            claimed.reference);
  REQUIRE(after.has_value());
  REQUIRE(after->waiter_count == 0);
  REQUIRE(after->planner_pin_count == 0);
  REQUIRE(after->reader_pin_count == 0);

  REQUIRE(release_reusable_reference(fixture.attachment, dead_waiter.token) ==
          ReusableRegistryErrorV1::StaleReference);
  REQUIRE(release_reusable_reference(fixture.attachment, *dead_planner) ==
          ReusableRegistryErrorV1::StaleReference);
  REQUIRE(release_reusable_reference(fixture.attachment, dead_reader.token) ==
          ReusableRegistryErrorV1::StaleReference);
  REQUIRE(validate_reusable_registry(fixture.attachment) ==
          ReusableRegistryErrorV1::None);

  REQUIRE(invalidate_reusable_generation(fixture.attachment,
                                         claimed.reference) ==
          ReusableRegistryErrorV1::None);
  const auto retirement =
      janitor_reusable_registry(fixture.attachment, 1'001);
  REQUIRE(retirement.error == ReusableRegistryErrorV1::None);
  REQUIRE(retirement.retired_invalidated == 1);
}

TEST_CASE("reusable producer diagnostics are bounded ABA-owned and separate") {
  RegistryFixture fixture{4};
  const auto key = coverage_key(46);
  auto claimed = claim(
      fixture, key, digest(68), "diagnostic-domain",
      "diagnostic-generation", 8, 101);
  REQUIRE(claimed);

  const std::string_view sql =
      "SELECT payload FROM provider_rows WHERE private_value = 17";
  const std::string_view planned =
      "Custom Scan (GenericProvider)\n  Provider Workers: 4";
  ReusableDiagnosticTextUpdateV1 update{
      .reference = claimed.reference,
      .producer = producer(8),
      .display_sql = char_view(sql),
      .producer_plan = char_view(planned),
      .plan_status = ReusablePlanSnapshotStatusV1::Planned,
      .producer_plan_truncated = true,
  };
  REQUIRE(
      store_reusable_diagnostic_text(fixture.attachment, update)
      == ReusableRegistryErrorV1::InvalidState);

  auto wrong_owner = update;
  wrong_owner.producer = producer(9);
  REQUIRE(
      begin_reusable_build_with_diagnostics(
          fixture.attachment, wrong_owner)
      == ReusableRegistryErrorV1::InvalidState);
  const auto still_claimed = snapshot_reusable_generation(
      fixture.attachment, claimed.reference);
  REQUIRE(still_claimed.has_value());
  REQUIRE(still_claimed->state == CacheEntryState::Claimed);

  auto premature_analyzed = update;
  premature_analyzed.plan_status =
      ReusablePlanSnapshotStatusV1::Analyzed;
  REQUIRE(
      begin_reusable_build_with_diagnostics(
          fixture.attachment, premature_analyzed)
      == ReusableRegistryErrorV1::InvalidArgument);
  DiagnosticsPartitionTrace begin_trace{};
  REQUIRE(
      begin_reusable_build_with_diagnostics(
          fixture.attachment, update,
          ReusableRegistryPartitionCallbacksV1{
              .context = &begin_trace,
              .acquire_exact = trace_exact_acquire,
              .release_exact = trace_exact_release,
              .acquire_coverage = trace_coverage_acquire,
              .release_coverage = trace_coverage_release,
          })
      == ReusableRegistryErrorV1::None);
  REQUIRE(begin_trace.exact_acquires == 1);
  REQUIRE(begin_trace.exact_releases == 1);
  REQUIRE(begin_trace.coverage_acquires == 0);
  REQUIRE(begin_trace.coverage_releases == 0);
  REQUIRE(
      begin_reusable_build_with_diagnostics(fixture.attachment, update)
      == ReusableRegistryErrorV1::InvalidState);

  std::array<ReusableGenerationDiagnosticsRowV1, 1> redacted_rows{};
  const auto redacted = snapshot_reusable_generation_diagnostics(
      fixture.attachment, redacted_rows);
  REQUIRE(redacted.error == ReusableRegistryErrorV1::None);
  REQUIRE(redacted.count == 1);
  REQUIRE(
      redacted_rows[0].plan_snapshot_status
      == ReusablePlanSnapshotStatusV1::Planned);
  REQUIRE_FALSE(redacted_rows[0].display_sql_truncated);
  REQUIRE(redacted_rows[0].producer_plan_truncated);

  std::array<char, kReusableDisplaySqlCapacityV1> sql_copy{};
  std::array<char, kReusableProducerPlanCapacityV1> plan_copy{};
  const auto copied = copy_reusable_diagnostic_text(
      fixture.attachment, claimed.reference, sql_copy, plan_copy);
  REQUIRE(copied.has_value());
  REQUIRE(copied->display_sql_bytes == sql.size());
  REQUIRE(copied->producer_plan_bytes == planned.size());
  REQUIRE(copied->plan_status == ReusablePlanSnapshotStatusV1::Planned);
  REQUIRE(copied->producer_plan_truncated);
  REQUIRE(
      std::string_view(sql_copy.data(), copied->display_sql_bytes) == sql);
  REQUIRE(
      std::string_view(plan_copy.data(), copied->producer_plan_bytes)
      == planned);

  std::array<char, 1> undersized{};
  REQUIRE(
      copy_reusable_diagnostic_text(
          fixture.attachment, claimed.reference, undersized, plan_copy)
          .error()
      == ReusableRegistryErrorV1::InvalidArgument);
  std::string oversized(kReusableProducerPlanCapacityV1 + 1U, 'x');
  auto oversized_update = update;
  oversized_update.producer_plan = char_view(oversized);
  REQUIRE(
      store_reusable_diagnostic_text(fixture.attachment, oversized_update)
      == ReusableRegistryErrorV1::InvalidArgument);

  const ReusableManifestHandleV1 manifest{
      .slot = claimed.reference.generation_slot,
      .slot_sequence = claimed.reference.slot_sequence + 100,
      .id = claimed.reference.id,
  };
  REQUIRE(
      publish_reusable_generation(
          fixture.attachment,
          ReusablePublishRequestV1{
              .reference = claimed.reference,
              .manifest = manifest,
              .completed_monotonic_ns = 202,
              .row_count = 4,
              .logical_bytes = 700,
              .stored_bytes = 300,
          })
      == ReusableRegistryErrorV1::None);
  const std::string_view analyzed =
      "Custom Scan (GenericProvider) (actual rows=4)";
  auto analyzed_update = update;
  analyzed_update.producer_plan = char_view(analyzed);
  analyzed_update.plan_status = ReusablePlanSnapshotStatusV1::Analyzed;
  analyzed_update.producer_plan_truncated = false;
  REQUIRE(
      store_reusable_diagnostic_text(fixture.attachment, analyzed_update)
      == ReusableRegistryErrorV1::None);
  REQUIRE(
      store_reusable_diagnostic_text(fixture.attachment, update)
      == ReusableRegistryErrorV1::InvalidState);

  auto stale_reference = claimed.reference;
  ++stale_reference.slot_sequence;
  REQUIRE(
      copy_reusable_diagnostic_text(
          fixture.attachment, stale_reference, sql_copy, plan_copy)
          .error()
      == ReusableRegistryErrorV1::StaleReference);
}

TEST_CASE("reusable legacy begin publishes a bounded unavailable marker") {
  RegistryFixture fixture{2};
  auto claimed = claim(
      fixture, coverage_key(48), digest(71), "legacy-domain",
      "legacy-generation", 7);
  REQUIRE(claimed);
  REQUIRE(
      begin_reusable_build(fixture.attachment, claimed.reference)
      == ReusableRegistryErrorV1::None);

  std::array<char, kReusableDisplaySqlCapacityV1> sql_copy{};
  std::array<char, kReusableProducerPlanCapacityV1> plan_copy{};
  const auto copied = copy_reusable_diagnostic_text(
      fixture.attachment, claimed.reference, sql_copy, plan_copy);
  REQUIRE(copied.has_value());
  REQUIRE(
      copied->plan_status == ReusablePlanSnapshotStatusV1::Unavailable);
  REQUIRE(
      std::string_view(sql_copy.data(), copied->display_sql_bytes)
      == "SQL capture unavailable");
  REQUIRE(
      std::string_view(plan_copy.data(), copied->producer_plan_bytes)
      == "plan capture unavailable");
  REQUIRE(
      validate_reusable_registry(fixture.attachment)
      == ReusableRegistryErrorV1::None);
}

TEST_CASE("reusable diagnostics copy bounded redacted generation rows") {
  RegistryFixture fixture{8, 32U << 10, 1};
  const auto key = coverage_key(47);

  auto claimed = claim(
      fixture, key, digest(70), "admin-domain", "claimed", 1,
      101, 60'001, "provider-private-secret");
  REQUIRE(claimed);
  auto waiter = claim(
      fixture, key, digest(70), "admin-domain", "claimed", 10);
  REQUIRE(waiter.disposition ==
          ReusableClaimDispositionV1::ExistingInFlight);

  auto building = claim(
      fixture, key, digest(71), "admin-domain", "building", 2, 102);
  REQUIRE(building);
  REQUIRE(begin_reusable_build(fixture.attachment, building.reference) ==
          ReusableRegistryErrorV1::None);

  auto complete = claim(
      fixture, key, digest(72), "admin-domain", "complete", 3, 103);
  REQUIRE(complete);
  const ReusableManifestHandleV1 complete_manifest =
      publish(fixture, complete.reference, 203);
  REQUIRE(static_cast<bool>(complete_manifest));
  auto planner_pin = pin_reusable_generation(
      fixture.attachment, complete.reference,
      ReusableReferenceKindV1::PlannerPin, reference_owner(700));
  REQUIRE(planner_pin.has_value());
  auto reader = lookup_reusable_current(
      fixture.attachment, key, byte_view("admin-domain"), digest(72),
      byte_view("complete"), ReusableLookupModeV1::PinComplete,
      reference_owner(701));
  REQUIRE(reader.error == ReusableRegistryErrorV1::None);

  auto failed = claim(
      fixture, key, digest(73), "admin-domain", "failed", 4, 104);
  REQUIRE(failed);
  REQUIRE(begin_reusable_build(fixture.attachment, failed.reference) ==
          ReusableRegistryErrorV1::None);
  REQUIRE(fail_reusable_generation(
              fixture.attachment, failed.reference,
              CacheFailureCode::ProducerError) ==
          ReusableRegistryErrorV1::None);

  std::array<ReusableGenerationDiagnosticsRowV1, 4> rows{};
  DiagnosticsPartitionTrace trace{};
  const auto snapshot = snapshot_reusable_generation_diagnostics(
      fixture.attachment, rows,
      ReusableRegistryPartitionCallbacksV1{
          .context = &trace,
          .acquire_exact = trace_exact_acquire,
          .release_exact = trace_exact_release,
          .acquire_coverage = trace_coverage_acquire,
          .release_coverage = trace_coverage_release,
      });
  REQUIRE(snapshot.error == ReusableRegistryErrorV1::None);
  REQUIRE(snapshot.indexed_generation_count == rows.size());
  REQUIRE(snapshot.count == rows.size());
  REQUIRE_FALSE(snapshot.truncated);
  REQUIRE(trace.exact_acquires == rows.size());
  REQUIRE(trace.exact_releases == rows.size());
  REQUIRE(trace.coverage_acquires == 0);
  REQUIRE(trace.coverage_releases == 0);

  const auto *claimed_row = diagnostics_row(rows, claimed.reference);
  REQUIRE(claimed_row != nullptr);
  REQUIRE(claimed_row->entry_kind == CacheEntryKind::ReusableCoverage);
  REQUIRE(claimed_row->state == CacheEntryState::Claimed);
  REQUIRE(claimed_row->failure == CacheFailureCode::None);
  REQUIRE(claimed_row->producer_pid == producer(1).pid);
  REQUIRE(claimed_row->waiter_count == 1);
  REQUIRE(claimed_row->provider_id == 7);
  REQUIRE(claimed_row->provider_abi_version == 2);
  REQUIRE(claimed_row->provider_domain_abi_version == 3);
  REQUIRE(claimed_row->dependency_count == 2);
  REQUIRE(claimed_row->database_oid == 11);
  REQUIRE(claimed_row->current_role_oid == 47);
  REQUIRE(claimed_row->dependency_fingerprint == digest(104));
  REQUIRE(claimed_row->compatibility_family_id == 41);
  REQUIRE(claimed_row->capability_mask == 0b1111);
  REQUIRE(claimed_row->observability == WriteObservability::ProviderComplete);
  REQUIRE(claimed_row->estimated_covered_rows == 50);
  REQUIRE(claimed_row->producer_ttl_ms == 60'001);
  REQUIRE(claimed_row->started_monotonic_ns == 101);

  const auto *building_row = diagnostics_row(rows, building.reference);
  REQUIRE(building_row != nullptr);
  REQUIRE(building_row->state == CacheEntryState::Building);
  REQUIRE(building_row->failure == CacheFailureCode::None);

  const auto *complete_row = diagnostics_row(rows, complete.reference);
  REQUIRE(complete_row != nullptr);
  REQUIRE(complete_row->state == CacheEntryState::Complete);
  REQUIRE(complete_row->manifest == complete_manifest);
  REQUIRE(complete_row->row_count == 17);
  REQUIRE(complete_row->logical_bytes == 700);
  REQUIRE(complete_row->stored_bytes == 300);
  REQUIRE(complete_row->planner_pin_count == 1);
  REQUIRE(complete_row->reader_pin_count == 1);
  REQUIRE(complete_row->completed_monotonic_ns == 203);
  REQUIRE(complete_row->last_access_monotonic_ns == 203);

  const auto *failed_row = diagnostics_row(rows, failed.reference);
  REQUIRE(failed_row != nullptr);
  REQUIRE(failed_row->state == CacheEntryState::Failed);
  REQUIRE(failed_row->failure == CacheFailureCode::ProducerError);

  std::array<ReusableGenerationDiagnosticsRowV1, 2> bounded_rows{};
  const auto bounded = snapshot_reusable_generation_diagnostics(
      fixture.attachment, bounded_rows);
  REQUIRE(bounded.error == ReusableRegistryErrorV1::None);
  REQUIRE(bounded.indexed_generation_count == rows.size());
  REQUIRE(bounded.count == bounded_rows.size());
  REQUIRE(bounded.truncated);

  const auto count_only = snapshot_reusable_generation_diagnostics(
      fixture.attachment,
      std::span<ReusableGenerationDiagnosticsRowV1>{});
  REQUIRE(count_only.error == ReusableRegistryErrorV1::None);
  REQUIRE(count_only.indexed_generation_count == rows.size());
  REQUIRE(count_only.count == 0);
  REQUIRE(count_only.truncated);

  REQUIRE(release_reusable_reference(fixture.attachment, waiter.token) ==
          ReusableRegistryErrorV1::None);
  REQUIRE(release_reusable_reference(fixture.attachment, *planner_pin) ==
          ReusableRegistryErrorV1::None);
  REQUIRE(release_reusable_reference(fixture.attachment, reader.token) ==
          ReusableRegistryErrorV1::None);
  REQUIRE(validate_reusable_registry(fixture.attachment) ==
          ReusableRegistryErrorV1::None);
}

TEST_CASE("bounded reusable invalidation is collision and ABA safe") {
  RegistryFixture fixture{8, 32U << 10, 1};
  const auto key = coverage_key(60);
  const auto other_security_key = coverage_key(61);
  const auto other_database_key = coverage_key(60, 90, 12);
  const CacheDigest256 forced_digest = digest(70);

  ReusableGenerationMetadataV1 shard_a_metadata = metadata();
  shard_a_metadata.physical_identity = physical_identity(10);
  ReusableGenerationMetadataV1 shard_b_metadata = shard_a_metadata;
  shard_b_metadata.physical_identity = physical_identity(11);
  ReusableGenerationMetadataV1 other_provider_metadata = shard_a_metadata;
  other_provider_metadata.provider_id = 8;

  auto shard_a = claim_with_metadata(fixture, key, forced_digest, "domain",
                                     "shard-a", 1, shard_a_metadata);
  auto shard_b = claim_with_metadata(fixture, key, forced_digest, "domain",
                                     "shard-b", 2, shard_b_metadata);
  auto other_security = claim_with_metadata(
      fixture, other_security_key, forced_digest, "domain", "other-security",
      3, shard_a_metadata);
  auto other_database = claim_with_metadata(
      fixture, other_database_key, forced_digest, "domain", "other-database",
      4, shard_a_metadata);
  auto other_provider = claim_with_metadata(
      fixture, key, forced_digest, "domain", "other-provider", 5,
      other_provider_metadata);
  auto inflight = claim_with_metadata(fixture, key, forced_digest, "domain",
                                      "inflight", 6, shard_a_metadata);
  REQUIRE(shard_a);
  REQUIRE(shard_b);
  REQUIRE(other_security);
  REQUIRE(other_database);
  REQUIRE(other_provider);
  REQUIRE(inflight);
  REQUIRE(static_cast<bool>(publish(fixture, shard_a.reference)));
  REQUIRE(static_cast<bool>(publish(fixture, shard_b.reference)));
  REQUIRE(static_cast<bool>(publish(fixture, other_security.reference)));
  REQUIRE(static_cast<bool>(publish(fixture, other_database.reference)));
  REQUIRE(static_cast<bool>(publish(fixture, other_provider.reference)));
  REQUIRE(begin_reusable_build(fixture.attachment, inflight.reference) ==
          ReusableRegistryErrorV1::None);

  auto pinned_reader = lookup_reusable_current(
      fixture.attachment, key, byte_view("domain"), forced_digest,
      byte_view("shard-a"), ReusableLookupModeV1::PinComplete,
      reference_owner(500));
  REQUIRE(pinned_reader.error == ReusableRegistryErrorV1::None);

  ReusableInvalidationSelectorV1 selector{
      .database_oid = key.security_namespace.database_oid,
      .provider_id = shard_a_metadata.provider_id,
      .security_namespace = key.security_namespace,
      .physical_identity = shard_a_metadata.physical_identity,
      .dependency_fingerprint = shard_a_metadata.dependency_fingerprint,
      .schema_fingerprint = shard_a_metadata.schema_fingerprint,
      .mapping_fingerprint = shard_a_metadata.mapping_fingerprint,
      .lineage_fingerprint = shard_a_metadata.lineage_fingerprint,
      .observability = WriteObservability::ProviderComplete,
      .maximum_scan = 2,
      .match_security_namespace = true,
      .match_provider_id = true,
      .match_physical_identity = true,
      .match_dependency_fingerprint = true,
      .match_schema_fingerprint = true,
      .match_mapping_fingerprint = true,
      .match_lineage_fingerprint = true,
      .match_observability = true,
  };
  std::vector<ReusableGenerationReferenceV1> affected;
  std::vector<ReusableManifestHandleV1> affected_manifests;
  std::vector<CacheEntryState> previous_states;
  std::uint32_t scanned = 0;
  std::uint32_t matched = 0;
  std::uint32_t invalidated_complete = 0;
  std::uint32_t failed_inflight = 0;
  std::uint32_t referenced = 0;
  std::uint32_t reader_pins = 0;
  bool done = false;
  while (!done) {
    const auto batch =
        invalidate_reusable_generations(fixture.attachment, selector);
    REQUIRE(batch.error == ReusableRegistryErrorV1::None);
    REQUIRE(batch.scanned_generations <= 2);
    REQUIRE(batch.next_cursor > selector.cursor);
    for (std::size_t index = 0; index < batch.affected_count; ++index) {
      affected.push_back(batch.affected_references[index]);
      affected_manifests.push_back(batch.affected_manifests[index]);
      previous_states.push_back(batch.previous_states[index]);
    }
    scanned += batch.scanned_generations;
    matched += batch.matched_generations;
    invalidated_complete += batch.invalidated_complete;
    failed_inflight += batch.failed_inflight;
    referenced += batch.referenced_generations;
    reader_pins += batch.reader_pins;
    selector.cursor = batch.next_cursor;
    done = batch.done;
  }
  REQUIRE(scanned == 8);
  REQUIRE(matched == 2);
  REQUIRE(invalidated_complete == 1);
  REQUIRE(failed_inflight == 1);
  REQUIRE(referenced == 1);
  REQUIRE(reader_pins == 1);
  REQUIRE(affected.size() == 2);
  REQUIRE(affected[0] == shard_a.reference);
  REQUIRE(affected[1] == inflight.reference);
  REQUIRE(affected_manifests[0].id == shard_a.reference.id);
  REQUIRE_FALSE(static_cast<bool>(affected_manifests[1]));
  REQUIRE(previous_states[0] == CacheEntryState::Complete);
  REQUIRE(previous_states[1] == CacheEntryState::Building);

  auto shard_a_after =
      snapshot_reusable_generation(fixture.attachment, shard_a.reference);
  auto inflight_after =
      snapshot_reusable_generation(fixture.attachment, inflight.reference);
  REQUIRE(shard_a_after->state == CacheEntryState::Invalidated);
  REQUIRE(shard_a_after->failure == CacheFailureCode::DependencyChanged);
  REQUIRE(inflight_after->state == CacheEntryState::Failed);
  REQUIRE(inflight_after->failure == CacheFailureCode::DependencyChanged);
  REQUIRE(snapshot_reusable_generation(fixture.attachment, shard_b.reference)
              ->state == CacheEntryState::Complete);
  REQUIRE(snapshot_reusable_generation(fixture.attachment,
                                       other_security.reference)
              ->state == CacheEntryState::Complete);
  REQUIRE(snapshot_reusable_generation(fixture.attachment,
                                       other_database.reference)
              ->state == CacheEntryState::Complete);
  REQUIRE(snapshot_reusable_generation(fixture.attachment,
                                       other_provider.reference)
              ->state == CacheEntryState::Complete);

  selector.cursor = 0;
  selector.maximum_scan = kMaximumReusableInvalidationScanV1;
  const auto repeated =
      invalidate_reusable_generations(fixture.attachment, selector);
  REQUIRE(repeated.error == ReusableRegistryErrorV1::None);
  REQUIRE(repeated.done);
  REQUIRE(repeated.matched_generations == 2);
  REQUIRE(repeated.already_invalidated == 1);
  REQUIRE(repeated.already_failed == 1);
  REQUIRE(repeated.affected_count == 0);
  REQUIRE(repeated.reader_pins == 1);

  auto pinned_retirement =
      retire_reusable_generation(fixture.attachment, shard_a.reference);
  REQUIRE_FALSE(pinned_retirement.has_value());
  REQUIRE(pinned_retirement.error() == ReusableRegistryErrorV1::InvalidState);
  REQUIRE(release_reusable_reference(fixture.attachment, pinned_reader.token) ==
          ReusableRegistryErrorV1::None);
  auto retired =
      retire_reusable_generation(fixture.attachment, shard_a.reference);
  REQUIRE(retired.has_value());
  REQUIRE(retired->id == shard_a.reference.id);
  auto reclaimed = reclaim_retired_reusable_generations(fixture.attachment, 1);
  REQUIRE(reclaimed.has_value());
  REQUIRE(*reclaimed == 1);
  auto replacement = claim_with_metadata(
      fixture, key, digest(71), "domain", "replacement", 7,
      shard_a_metadata);
  REQUIRE(replacement);
  REQUIRE(replacement.reference.generation_slot ==
          shard_a.reference.generation_slot);
  REQUIRE(replacement.reference.slot_sequence !=
          shard_a.reference.slot_sequence);
  auto stale =
      snapshot_reusable_generation(fixture.attachment, affected.front());
  REQUIRE_FALSE(stale.has_value());
  REQUIRE(stale.error() == ReusableRegistryErrorV1::StaleReference);
  REQUIRE(validate_reusable_registry(fixture.attachment) ==
          ReusableRegistryErrorV1::None);
}

TEST_CASE("reusable invalidation observability and coarse scope are explicit") {
  RegistryFixture fixture{4, 16U << 10, 1};
  const auto key = coverage_key(70, 91, 70);
  const auto other_database_key = coverage_key(70, 91, 71);
  ReusableGenerationMetadataV1 precise_metadata = metadata();
  precise_metadata.physical_identity = physical_identity(20);
  ReusableGenerationMetadataV1 other_provider_metadata = precise_metadata;
  other_provider_metadata.provider_id = 8;

  auto precise = claim_with_metadata(fixture, key, digest(80), "domain",
                                     "precise", 1, precise_metadata);
  auto other_provider = claim_with_metadata(
      fixture, key, digest(81), "domain", "other-provider", 2,
      other_provider_metadata);
  auto other_database = claim_with_metadata(
      fixture, other_database_key, digest(82), "domain", "other-database", 3,
      precise_metadata);
  REQUIRE(static_cast<bool>(publish(fixture, precise.reference)));
  REQUIRE(static_cast<bool>(publish(fixture, other_provider.reference)));
  REQUIRE(static_cast<bool>(publish(fixture, other_database.reference)));

  REQUIRE(invalidate_reusable_generations(
              fixture.attachment,
              ReusableInvalidationSelectorV1{.database_oid = 70})
              .error == ReusableRegistryErrorV1::InvalidArgument);

  const auto precise_result = invalidate_reusable_generations(
      fixture.attachment,
      ReusableInvalidationSelectorV1{
          .database_oid = 70,
          .provider_id = 7,
          .observability = WriteObservability::ProviderComplete,
          .match_provider_id = true,
          .match_observability = true,
      });
  REQUIRE(precise_result.error == ReusableRegistryErrorV1::None);
  REQUIRE(precise_result.invalidated_complete == 1);
  REQUIRE(precise_result.affected_references[0] == precise.reference);
  REQUIRE(snapshot_reusable_generation(fixture.attachment,
                                       other_provider.reference)
              ->state == CacheEntryState::Complete);

  ReusableInvalidationSelectorV1 ambiguous{
      .database_oid = 70,
      .provider_id = 7,
      .match_provider_id = true,
      .coarse_database_fallback = true,
  };
  REQUIRE(invalidate_reusable_generations(fixture.attachment, ambiguous)
              .error == ReusableRegistryErrorV1::InvalidArgument);

  const auto coarse_result = invalidate_reusable_generations(
      fixture.attachment,
      ReusableInvalidationSelectorV1{
          .database_oid = 70,
          .coarse_database_fallback = true,
      });
  REQUIRE(coarse_result.error == ReusableRegistryErrorV1::None);
  REQUIRE(coarse_result.matched_generations == 2);
  REQUIRE(coarse_result.invalidated_complete == 1);
  REQUIRE(coarse_result.already_invalidated == 1);
  REQUIRE(snapshot_reusable_generation(fixture.attachment,
                                       other_database.reference)
              ->state == CacheEntryState::Complete);
  REQUIRE(validate_reusable_registry(fixture.attachment) ==
          ReusableRegistryErrorV1::None);
}

TEST_CASE("janitor fails dead owners and retires expired manifests") {
  RegistryFixture fixture{4, 16U << 10, 1};
  const auto key = coverage_key(50);
  auto dead = claim(fixture, key, digest(50), "domain", "dead", 1);
  REQUIRE(dead);
  REQUIRE(begin_reusable_build(fixture.attachment, dead.reference) ==
          ReusableRegistryErrorV1::None);
  auto live_waiter = claim(fixture, key, digest(50), "domain", "dead", 2);
  REQUIRE(live_waiter.disposition ==
          ReusableClaimDispositionV1::ExistingInFlight);
  REQUIRE(live_waiter.token.kind == ReusableReferenceKindV1::Waiter);

  auto dead_sweep = janitor_reusable_registry(
      fixture.attachment, 1'000, kMaximumReusableJanitorBatchV1,
      ReusableJanitorCallbacksV1{
          .producer_alive = producer_is_dead,
      });
  REQUIRE(dead_sweep.error == ReusableRegistryErrorV1::None);
  REQUIRE(dead_sweep.failed_dead_producers == 1);
  REQUIRE(dead_sweep.retired_failed == 0);
  REQUIRE(dead_sweep.retired_manifest_count == 0);
  REQUIRE(dead_sweep.affected_reference_count == 1);
  REQUIRE(dead_sweep.affected_references[0] == dead.reference);
  const auto failed = snapshot_reusable_generation(
      fixture.attachment, dead.reference);
  REQUIRE(failed.has_value());
  REQUIRE(failed->state == CacheEntryState::Failed);
  REQUIRE(failed->waiter_count == 1);
  REQUIRE(release_reusable_reference(
              fixture.attachment, live_waiter.token) ==
          ReusableRegistryErrorV1::None);

  const auto failed_retirement =
      janitor_reusable_registry(fixture.attachment, 1'001);
  REQUIRE(failed_retirement.error == ReusableRegistryErrorV1::None);
  REQUIRE(failed_retirement.retired_failed == 1);
  REQUIRE(failed_retirement.affected_reference_count == 1);
  REQUIRE(failed_retirement.affected_references[0] == dead.reference);
  REQUIRE(validate_reusable_registry(fixture.attachment) ==
          ReusableRegistryErrorV1::None);

  auto expiring =
      claim(fixture, key, digest(51), "domain", "expiring", 2, 100, 1);
  REQUIRE(expiring);
  const auto expiring_manifest = publish(fixture, expiring.reference, 200);
  REQUIRE(static_cast<bool>(expiring_manifest));
  auto expiry_sweep = janitor_reusable_registry(fixture.attachment, 2'000'000);
  REQUIRE(expiry_sweep.error == ReusableRegistryErrorV1::None);
  REQUIRE(expiry_sweep.reclaimed_retired == 1);
  REQUIRE(expiry_sweep.retired_expired == 1);
  REQUIRE(expiry_sweep.retired_manifest_count == 1);
  REQUIRE(expiry_sweep.retired_manifests[0].id == expiring.reference.id);
  REQUIRE(expiry_sweep.affected_reference_count == 1);
  REQUIRE(expiry_sweep.affected_references[0] == expiring.reference);
  REQUIRE(validate_reusable_registry(fixture.attachment) ==
          ReusableRegistryErrorV1::None);

  auto final_sweep = janitor_reusable_registry(fixture.attachment, 2'000'001);
  REQUIRE(final_sweep.error == ReusableRegistryErrorV1::None);
  REQUIRE(final_sweep.reclaimed_retired == 1);
  const auto snapshot = reusable_registry_snapshot(fixture.attachment);
  REQUIRE(snapshot.indexed_generations == 0);
  REQUIRE(snapshot.retired_generations == 0);
  REQUIRE(snapshot.free_generations == 4);
  REQUIRE(snapshot.arena_free_bytes == 16U << 10);
  REQUIRE(validate_reusable_registry(fixture.attachment) ==
          ReusableRegistryErrorV1::None);
}

TEST_CASE("janitor wake references are bounded and deduplicated") {
  RegistryFixture fixture{8, 32U << 10, 1};
  const auto key = coverage_key(52);
  std::array<ReusableGenerationReferenceV1, 4> references{};
  for (std::size_t index = 0; index < references.size(); ++index) {
    const std::string generation = "dead-" + std::to_string(index);
    auto claimed = claim(
        fixture, key,
        digest(static_cast<std::uint8_t>(60 + index)),
        "domain", generation, static_cast<std::uint32_t>(index + 1));
    REQUIRE(claimed);
    REQUIRE(begin_reusable_build(fixture.attachment, claimed.reference) ==
            ReusableRegistryErrorV1::None);
    references[index] = claimed.reference;
  }

  constexpr std::uint32_t maximum = 3;
  const auto sweep = janitor_reusable_registry(
      fixture.attachment, 1'000, maximum,
      ReusableJanitorCallbacksV1{
          .producer_alive = producer_is_dead,
      });
  REQUIRE(sweep.error == ReusableRegistryErrorV1::None);
  REQUIRE(sweep.failed_dead_producers == maximum);
  REQUIRE(sweep.retired_failed == maximum);
  REQUIRE(sweep.affected_reference_count == maximum);
  REQUIRE(sweep.affected_reference_count <= sweep.affected_references.size());
  for (std::size_t index = 0; index < sweep.affected_reference_count;
       ++index) {
    REQUIRE(std::find(references.begin(), references.end(),
                      sweep.affected_references[index]) != references.end());
    for (std::size_t prior = 0; prior < index; ++prior) {
      REQUIRE(sweep.affected_references[prior] !=
              sweep.affected_references[index]);
    }
  }
  REQUIRE(validate_reusable_registry(fixture.attachment) ==
          ReusableRegistryErrorV1::None);
}
