#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
claim(RegistryFixture &fixture, const ReusableCoverageDirectoryKeyV1 &key,
      const CacheDigest256 &generation_digest, std::string_view domain,
      std::string_view generation, std::uint32_t producer_ordinal,
      std::uint64_t started_ns = 100,
      std::uint64_t producer_ttl_ms = 60'000) noexcept {
  return claim_reusable_generation(
      fixture.attachment, ReusableClaimRequestV1{
                              .coverage_directory_key = key,
                              .generation_digest = generation_digest,
                              .canonical_domain_key = byte_view(domain),
                              .canonical_generation_key = byte_view(generation),
                              .metadata = metadata(producer_ttl_ms),
                              .producer = producer(producer_ordinal),
                              .started_monotonic_ns = started_ns,
                          });
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
      .provider_id = 7,
      .compatibility_family_id = 41,
      .required_capability_mask = 0b0101,
  };
}

[[nodiscard]] bool
producer_is_dead(void *, const ReusableProducerIdentityV1 &) noexcept {
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
      byte_view("generation-a"), ReusableLookupModeV1::PinComplete);
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

TEST_CASE("two index retirement and reclaim reject stale ABA references") {
  RegistryFixture fixture{1, 4U << 10, 1};
  const auto key = coverage_key(40);
  auto claimed = claim(fixture, key, digest(40), "domain", "generation", 1);
  REQUIRE(claimed);
  const auto old_reference = claimed.reference;
  const auto manifest = publish(fixture, old_reference);
  REQUIRE(static_cast<bool>(manifest));

  auto planner_pin = pin_reusable_generation(
      fixture.attachment, old_reference, ReusableReferenceKindV1::PlannerPin);
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
      byte_view("generation"), ReusableLookupModeV1::Observe);
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

TEST_CASE("janitor fails dead owners and retires expired manifests") {
  RegistryFixture fixture{4, 16U << 10, 1};
  const auto key = coverage_key(50);
  auto dead = claim(fixture, key, digest(50), "domain", "dead", 1);
  REQUIRE(dead);
  REQUIRE(begin_reusable_build(fixture.attachment, dead.reference) ==
          ReusableRegistryErrorV1::None);

  auto dead_sweep = janitor_reusable_registry(
      fixture.attachment, 1'000, kMaximumReusableJanitorBatchV1,
      ReusableJanitorCallbacksV1{
          .producer_alive = producer_is_dead,
      });
  REQUIRE(dead_sweep.error == ReusableRegistryErrorV1::None);
  REQUIRE(dead_sweep.failed_dead_producers == 1);
  REQUIRE(dead_sweep.retired_failed == 1);
  REQUIRE(dead_sweep.retired_manifest_count == 0);
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
