#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
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

[[nodiscard]] std::span<const std::byte> byte_view(std::string_view value) noexcept {
    return {
        reinterpret_cast<const std::byte*>(value.data()),
        value.size(),
    };
}

[[nodiscard]] std::span<const char> char_view(std::string_view value) noexcept {
    return {value.data(), value.size()};
}

[[nodiscard]] cache_digest_256 digest(std::uint8_t seed) noexcept {
    cache_digest_256 result{};
    for (std::size_t index = 0; index < result.bytes.size(); ++index) {
        result.bytes[index] = static_cast<std::uint8_t>(seed + index);
    }
    return result;
}

inline constexpr std::size_t k_test_dependency_count = 2;
using test_dependency_envelope =
    std::array<std::byte, dependency_vector_envelope_bytes(k_test_dependency_count)>;

[[nodiscard]] dependency_vector_entry dependency_entry(std::uint32_t object_oid) noexcept {
    return dependency_vector_entry{
        .version =
            {
                .key =
                    {
                        .database_oid = 11,
                        .class_oid = 1'259,
                        .object_oid = object_oid,
                        .kind = dependency_kind::relation_content,
                    },
                .committed_epoch = object_oid + 10U,
                .physical_identity =
                    {
                        .kind = physical_identity_kind::none,
                    },
                .observability = write_observability::executor_utility_only,
            },
        .record_generation = object_oid + 100U,
    };
}

[[nodiscard]] test_dependency_envelope dependency_envelope() noexcept {
    const std::array<dependency_vector_entry, k_test_dependency_count> entries{
        dependency_entry(100),
        dependency_entry(101),
    };
    test_dependency_envelope envelope{};
    static_cast<void>(initialize_dependency_vector_envelope(envelope, entries));
    return envelope;
}

[[nodiscard]] reusable_conservative_guard conservative_guard() noexcept {
    return reusable_conservative_guard{
        .dependency =
            {
                .version =
                    {
                        .key =
                            {
                                .database_oid = 11,
                                .object_oid = 11,
                                .kind = dependency_kind::database_write_epoch,
                            },
                        .committed_epoch = 17,
                        .observability = write_observability::database_wide_fallback,
                    },
                .record_generation = 211,
            },
        .snapshot_completion_count = 313,
        .active = true,
    };
}

[[nodiscard]] reusable_conservative_guard definition_guard() noexcept {
    reusable_conservative_guard result = conservative_guard();
    result.dependency.version.key.kind = dependency_kind::database_definition_epoch;
    result.snapshot_completion_count = 0;
    return result;
}

[[nodiscard]] reusable_coverage_directory_key coverage_key(std::uint32_t role,
                                                           std::uint8_t domain_seed = 90) noexcept {
    return reusable_coverage_directory_key{
        .security_namespace =
            {
                .database_oid = 11,
                .current_role_oid = role,
                .session_user_oid = role,
                .security_context_flags = 3,
                .row_security_mode = 1,
                .provider_security_digest = digest(static_cast<std::uint8_t>(role)),
            },
        .coverage_domain_digest = digest(domain_seed),
    };
}

[[nodiscard]] reusable_coverage_directory_key
coverage_key(std::uint32_t role, std::uint8_t domain_seed, std::uint32_t database_oid) noexcept {
    reusable_coverage_directory_key result = coverage_key(role, domain_seed);
    result.security_namespace.database_oid = database_oid;
    return result;
}

[[nodiscard]] reusable_coverage_directory_key exact_key(std::uint32_t role,
                                                        std::uint32_t database_oid = 11) noexcept {
    reusable_coverage_directory_key result = coverage_key(role, 0, database_oid);
    result.coverage_domain_digest = cache_digest_256{};
    return result;
}

[[nodiscard]] reusable_generation_metadata
metadata(std::uint64_t producer_ttl_ms = 60'000) noexcept {
    return reusable_generation_metadata{
        .provider_id = 7,
        .provider_abi_revision = 2,
        .provider_domain_abi_revision = 3,
        .dependency_count = 2,
        .compatibility_family_id = 41,
        .capability_mask = 0b1111,
        .schema_fingerprint = digest(101),
        .mapping_fingerprint = digest(102),
        .lineage_fingerprint = digest(103),
        .dependency_fingerprint = digest(104),
        .observability = write_observability::provider_complete,
        .estimated_covered_rows = 50,
        .producer_ttl_ms = producer_ttl_ms,
    };
}

[[nodiscard]] reusable_generation_metadata
exact_metadata(std::uint64_t producer_ttl_ms = 60'000) noexcept {
    return reusable_generation_metadata{
        .dependency_count = 2,
        .schema_fingerprint = digest(111),
        .dependency_fingerprint = digest(112),
        .observability = write_observability::database_wide_fallback,
        .producer_ttl_ms = producer_ttl_ms,
    };
}

[[nodiscard]] reusable_producer_identity producer(std::uint32_t ordinal) noexcept {
    return reusable_producer_identity{
        .proc_number = ordinal,
        .pid = static_cast<std::int32_t>(10'000 + ordinal),
        .backend_nonce = 20'000 + ordinal,
        .backend_started_at_us = 30'000 + ordinal,
    };
}

[[nodiscard]] physical_identity physical_identity(std::uint8_t seed) noexcept {
    physical_identity result{
        .kind = physical_identity_kind::provider_opaque,
        .length = 4,
    };
    for (std::size_t index = 0; index < result.length; ++index) {
        result.bytes[index] = std::byte{static_cast<std::uint8_t>(seed + index)};
    }
    return result;
}

[[nodiscard]] reusable_reference_owner reference_owner(std::uint32_t ordinal) noexcept {
    return producer(1'000 + ordinal);
}

template <class p_row>
concept exposes_reusable_shared_byte_ranges = requires(p_row value) {
    value.canonical_generation_key;
    value.canonical_domain_key;
    value.coverage_descriptor;
};

static_assert(!exposes_reusable_shared_byte_ranges<reusable_generation_diagnostics_row>);

template <class p_row>
concept exposes_reusable_diagnostic_text = requires(p_row value) {
    value.display_sql;
    value.canonical_plan;
    value.producer_plan;
};

static_assert(!exposes_reusable_diagnostic_text<reusable_generation_diagnostics_row>);

struct diagnostics_partition_trace {
    std::uint32_t exact_acquires = 0;
    std::uint32_t exact_releases = 0;
    std::uint32_t coverage_acquires = 0;
    std::uint32_t coverage_releases = 0;
};

void trace_exact_acquire(void* opaque, std::uint32_t) noexcept {
    auto& trace = *static_cast<diagnostics_partition_trace*>(opaque);
    ++trace.exact_acquires;
}

void trace_exact_release(void* opaque, std::uint32_t) noexcept {
    auto& trace = *static_cast<diagnostics_partition_trace*>(opaque);
    ++trace.exact_releases;
}

void trace_coverage_acquire(void* opaque, std::uint32_t) noexcept {
    auto& trace = *static_cast<diagnostics_partition_trace*>(opaque);
    ++trace.coverage_acquires;
}

void trace_coverage_release(void* opaque, std::uint32_t) noexcept {
    auto& trace = *static_cast<diagnostics_partition_trace*>(opaque);
    ++trace.coverage_releases;
}

[[nodiscard]] const reusable_generation_diagnostics_row*
diagnostics_row(std::span<const reusable_generation_diagnostics_row> rows,
                const reusable_generation_reference& reference) noexcept {
    for (const reusable_generation_diagnostics_row& row : rows) {
        if (row.reference == reference) {
            return &row;
        }
    }
    return nullptr;
}

class registry_fixture {
  private:
    reusable_registry_config config_{};
    std::vector<std::uint64_t> region_{};

  public:
    explicit registry_fixture(std::uint32_t max_generations,
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
            reinterpret_cast<std::byte*>(region_.data()),
            request_.layout.total_bytes,
        };
        auto started = startup_reusable_registry(bytes, k_boot_nonce, config_);
        if (!started.has_value()) {
            throw std::runtime_error("registry startup failed");
        }
        attachment = *started;
    }

    static constexpr std::uint64_t k_boot_nonce = 0xB105E0ULL;
    reusable_registry_attachment attachment{};
    reusable_registry_preload_request request_{};
};

[[nodiscard]] reusable_shared_generation&
shared_generation(registry_fixture& fixture,
                  const reusable_generation_reference& reference) noexcept {
    auto* records = reinterpret_cast<reusable_shared_generation*>(
        fixture.attachment.region.data() + fixture.request_.layout.generations_offset);
    return records[reference.generation_slot];
}

[[nodiscard]] constexpr std::uint64_t aligned_eight(std::size_t bytes) noexcept {
    return (static_cast<std::uint64_t>(bytes) + 7U) & ~std::uint64_t{7U};
}

struct diagnostic_aba_trace {
    reusable_shared_generation* generation = nullptr;
    std::uint32_t exact_acquires = 0;
    std::uint32_t exact_releases = 0;
    bool mutate_on_first_release = false;
};

void diagnostic_aba_acquire(void* opaque, std::uint32_t) noexcept {
    auto& trace = *static_cast<diagnostic_aba_trace*>(opaque);
    ++trace.exact_acquires;
}

void diagnostic_aba_release(void* opaque, std::uint32_t) noexcept {
    auto& trace = *static_cast<diagnostic_aba_trace*>(opaque);
    ++trace.exact_releases;
    if (trace.mutate_on_first_release && trace.exact_releases == 1 && trace.generation != nullptr) {
        ++trace.generation->slot_sequence;
    }
}

void diagnostic_aba_coverage_noop(void*, std::uint32_t) noexcept {}

[[nodiscard]] reusable_claim_result
claim_with_metadata(registry_fixture& fixture,
                    const reusable_coverage_directory_key& key,
                    const cache_digest_256& generation_digest,
                    std::string_view domain,
                    std::string_view generation,
                    std::uint32_t producer_ordinal,
                    const reusable_generation_metadata& generation_metadata,
                    std::uint64_t started_ns = 100,
                    std::string_view coverage_descriptor = {}) noexcept {
    const test_dependency_envelope dependencies = dependency_envelope();
    return claim_reusable_generation(fixture.attachment,
                                     reusable_claim_request{
                                         .coverage_directory_key = key,
                                         .generation_digest = generation_digest,
                                         .canonical_domain_key = byte_view(domain),
                                         .canonical_generation_key = byte_view(generation),
                                         .coverage_descriptor = byte_view(coverage_descriptor),
                                         .dependency_vector_envelope = dependencies,
                                         .metadata = generation_metadata,
                                         .producer = producer(producer_ordinal),
                                         .reference_owner = reference_owner(producer_ordinal),
                                         .started_monotonic_ns = started_ns,
                                     });
}

[[nodiscard]] reusable_claim_result claim(registry_fixture& fixture,
                                          const reusable_coverage_directory_key& key,
                                          const cache_digest_256& generation_digest,
                                          std::string_view domain,
                                          std::string_view generation,
                                          std::uint32_t producer_ordinal,
                                          std::uint64_t started_ns = 100,
                                          std::uint64_t producer_ttl_ms = 60'000,
                                          std::string_view coverage_descriptor = {}) noexcept {
    return claim_with_metadata(fixture,
                               key,
                               generation_digest,
                               domain,
                               generation,
                               producer_ordinal,
                               metadata(producer_ttl_ms),
                               started_ns,
                               coverage_descriptor);
}

[[nodiscard]] reusable_claim_result
claim_exact_with_guard(registry_fixture& fixture,
                       const reusable_coverage_directory_key& key,
                       const cache_digest_256& generation_digest,
                       std::string_view generation,
                       std::uint32_t producer_ordinal,
                       const reusable_conservative_guard& guard,
                       std::uint64_t started_ns = 100,
                       std::uint64_t producer_ttl_ms = 60'000) noexcept {
    const test_dependency_envelope dependencies = dependency_envelope();
    return claim_reusable_generation(fixture.attachment,
                                     reusable_claim_request{
                                         .entry_kind = cache_entry_kind::exact_final_result,
                                         .coverage_directory_key = key,
                                         .generation_digest = generation_digest,
                                         .canonical_generation_key = byte_view(generation),
                                         .dependency_vector_envelope = dependencies,
                                         .conservative_guard = guard,
                                         .metadata = exact_metadata(producer_ttl_ms),
                                         .producer = producer(producer_ordinal),
                                         .reference_owner = reference_owner(producer_ordinal),
                                         .started_monotonic_ns = started_ns,
                                     });
}

[[nodiscard]] reusable_claim_result claim_exact(registry_fixture& fixture,
                                                const reusable_coverage_directory_key& key,
                                                const cache_digest_256& generation_digest,
                                                std::string_view generation,
                                                std::uint32_t producer_ordinal,
                                                std::uint64_t started_ns = 100,
                                                std::uint64_t producer_ttl_ms = 60'000) noexcept {
    return claim_exact_with_guard(fixture,
                                  key,
                                  generation_digest,
                                  generation,
                                  producer_ordinal,
                                  conservative_guard(),
                                  started_ns,
                                  producer_ttl_ms);
}

[[nodiscard]] reusable_manifest_handle publish(registry_fixture& fixture,
                                               const reusable_generation_reference& reference,
                                               std::uint64_t completed_ns = 200) noexcept {
    if (begin_reusable_build(fixture.attachment, reference) != reusable_registry_error::none) {
        return {};
    }
    const reusable_manifest_handle handle{
        .slot = reference.generation_slot,
        .slot_sequence = reference.slot_sequence + 100,
        .id = reference.id,
    };
    if (publish_reusable_generation(fixture.attachment,
                                    reusable_publish_request{
                                        .reference = reference,
                                        .manifest = handle,
                                        .completed_monotonic_ns = completed_ns,
                                        .row_count = 17,
                                        .logical_bytes = 700,
                                        .stored_bytes = 300,
                                    }) != reusable_registry_error::none) {
        return {};
    }
    return handle;
}

[[nodiscard]] reusable_registry_error
release_and_finalize_one_retired(registry_fixture& fixture) noexcept {
    test_dependency_envelope copied{};
    const reusable_dependency_release_preparation prepared =
        prepare_reusable_dependency_release(fixture.attachment, copied);
    if (!prepared) {
        return prepared.error;
    }
    const std::uint32_t remaining =
        prepared.ticket.reference_count - prepared.ticket.first_reference;
    const auto progress =
        record_reusable_dependency_release_progress(fixture.attachment, prepared.ticket, remaining);
    if (!progress.has_value()) {
        return progress.error();
    }
    if (!*progress) {
        return reusable_registry_error::invalid_state;
    }
    const auto finalized =
        finalize_reusable_generation_reclaim(fixture.attachment, prepared.ticket);
    return finalized.has_value() ? reusable_registry_error::none : finalized.error();
}

[[nodiscard]] reusable_candidate_request
candidate_request(const reusable_coverage_directory_key& key, std::string_view domain) noexcept {
    return reusable_candidate_request{
        .coverage_directory_key = key,
        .canonical_domain_key = byte_view(domain),
        .reference_owner = reference_owner(key.security_namespace.current_role_oid),
        .provider_id = 7,
        .compatibility_family_id = 41,
        .required_capability_mask = 0b0101,
    };
}

[[nodiscard]] bool janitor_owner_is_dead(void*,
                                         const reusable_janitor_owner_observation&) noexcept {
    return false;
}

[[nodiscard]] bool
janitor_only_producers_are_alive(void*,
                                 const reusable_janitor_owner_observation& observation) noexcept {
    return observation.producer;
}

[[nodiscard]] bool janitor_producers_are_dead_references_are_alive(
    void*, const reusable_janitor_owner_observation& observation) noexcept {
    return !observation.producer;
}

struct sampled_owner_liveness {
    reusable_janitor_owner_observation observation{};
    bool alive = true;
};

[[nodiscard]] bool
sampled_owner_alive(void* opaque, const reusable_janitor_owner_observation& observation) noexcept {
    const auto& sample = *static_cast<const sampled_owner_liveness*>(opaque);
    return observation == sample.observation ? sample.alive : true;
}

} // namespace

TEST_CASE("reusable registry sizes exactly and coalesces only full keys",
          "[component:cache][layer:unit][feature:registry]") {
    REQUIRE(reusable_registry_config{}.max_generations == 1'024);
    registry_fixture fixture{8};
    REQUIRE(fixture.request_.membership_lock_count == 1);
    REQUIRE(fixture.request_.exact_partition_lock_count == 4);
    REQUIRE(fixture.request_.coverage_partition_lock_count == 4);
    REQUIRE(fixture.attachment.region.size() == fixture.request_.layout.total_bytes);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);

    const auto key = coverage_key(22);
    const cache_digest_256 forced_collision = digest(1);
    auto first = claim(fixture, key, forced_collision, "domain-a", "generation-a", 1);
    REQUIRE(first.disposition == reusable_claim_disposition::claimed_producer);

    auto same = claim(fixture, key, forced_collision, "domain-a", "generation-a", 2);
    REQUIRE(same.disposition == reusable_claim_disposition::existing_in_flight);
    REQUIRE(same.reference == first.reference);
    REQUIRE(same.token.active);
    REQUIRE(same.token.kind == reusable_reference_kind::waiter);

    auto digest_collision = claim(fixture, key, forced_collision, "domain-a", "generation-b", 3);
    REQUIRE(digest_collision.disposition == reusable_claim_disposition::claimed_producer);
    REQUIRE(digest_collision.reference != first.reference);
    REQUIRE(release_reusable_reference(fixture.attachment, same.token) ==
            reusable_registry_error::none);

    const auto handle = publish(fixture, first.reference);
    REQUIRE(static_cast<bool>(handle));
    auto lookup = lookup_reusable_generation(fixture.attachment,
                                             key,
                                             byte_view("domain-a"),
                                             forced_collision,
                                             byte_view("generation-a"),
                                             reusable_lookup_mode::pin_complete,
                                             reference_owner(90));
    REQUIRE(lookup.error == reusable_registry_error::none);
    REQUIRE(lookup.snapshot.state == cache_entry_state::complete);
    REQUIRE(lookup.snapshot.manifest.id == first.reference.id);
    REQUIRE(lookup.token.kind == reusable_reference_kind::reader_pin);
    REQUIRE(release_reusable_reference(fixture.attachment, lookup.token) ==
            reusable_registry_error::none);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("terminal publication snapshots every active waiter owner",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{4, 16U << 10, 1};
    auto producer_claim = claim_exact(fixture, exact_key(222), digest(222), "waiter-transfer", 1);
    REQUIRE(producer_claim.disposition == reusable_claim_disposition::claimed_producer);
    auto first_waiter = claim_exact(fixture, exact_key(222), digest(222), "waiter-transfer", 2);
    auto second_waiter = claim_exact(fixture, exact_key(222), digest(222), "waiter-transfer", 3);
    REQUIRE(first_waiter.token.kind == reusable_reference_kind::waiter);
    REQUIRE(second_waiter.token.kind == reusable_reference_kind::waiter);

    const reusable_waiter_owner_batch both =
        snapshot_reusable_waiter_owners(fixture.attachment, producer_claim.reference);
    REQUIRE(both);
    REQUIRE(both.count == 2);
    REQUIRE(std::find(both.waiters.begin(),
                      both.waiters.begin() + both.count,
                      first_waiter.token) != both.waiters.begin() + both.count);
    REQUIRE(std::find(both.waiters.begin(),
                      both.waiters.begin() + both.count,
                      second_waiter.token) != both.waiters.begin() + both.count);

    REQUIRE(release_reusable_reference(fixture.attachment, first_waiter.token) ==
            reusable_registry_error::none);
    const reusable_waiter_owner_batch remaining =
        snapshot_reusable_waiter_owners(fixture.attachment, producer_claim.reference);
    REQUIRE(remaining);
    REQUIRE(remaining.count == 1);
    REQUIRE(remaining.waiters[0] == second_waiter.token);

    reusable_generation_reference stale = producer_claim.reference;
    ++stale.slot_sequence;
    const reusable_waiter_owner_batch rejected =
        snapshot_reusable_waiter_owners(fixture.attachment, stale);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error == reusable_registry_error::stale_reference);
    REQUIRE(release_reusable_reference(fixture.attachment, second_waiter.token) ==
            reusable_registry_error::none);
}

TEST_CASE("unified registry separates exact and coverage digest collisions",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{8};
    const cache_digest_256 forced_collision = digest(201);
    const auto exact_namespace = exact_key(22);
    const auto reusable_namespace = coverage_key(22, 201);

    auto exact = claim_exact(fixture, exact_namespace, forced_collision, "same-canonical-bytes", 1);
    REQUIRE(exact.disposition == reusable_claim_disposition::claimed_producer);
    auto exact_waiter =
        claim_exact(fixture, exact_namespace, forced_collision, "same-canonical-bytes", 2);
    REQUIRE(exact_waiter.disposition == reusable_claim_disposition::existing_in_flight);
    REQUIRE(exact_waiter.reference == exact.reference);
    auto exact_other_security =
        claim_exact(fixture, exact_key(23), forced_collision, "same-canonical-bytes", 4);
    REQUIRE(exact_other_security.disposition == reusable_claim_disposition::claimed_producer);
    REQUIRE(exact_other_security.reference != exact.reference);

    auto reusable = claim(fixture,
                          reusable_namespace,
                          forced_collision,
                          "coverage-domain",
                          "same-canonical-bytes",
                          3);
    REQUIRE(reusable.disposition == reusable_claim_disposition::claimed_producer);
    REQUIRE(reusable.reference != exact.reference);

    auto exact_lookup = lookup_cache_generation(fixture.attachment,
                                                cache_entry_kind::exact_final_result,
                                                exact_namespace,
                                                std::span<const std::byte>{},
                                                forced_collision,
                                                byte_view("same-canonical-bytes"),
                                                reusable_lookup_mode::observe,
                                                reference_owner(90));
    REQUIRE(exact_lookup.error == reusable_registry_error::none);
    REQUIRE(exact_lookup.snapshot.reference == exact.reference);
    REQUIRE(exact_lookup.snapshot.entry_kind == cache_entry_kind::exact_final_result);
    auto other_security_lookup = lookup_cache_generation(fixture.attachment,
                                                         cache_entry_kind::exact_final_result,
                                                         exact_key(23),
                                                         std::span<const std::byte>{},
                                                         forced_collision,
                                                         byte_view("same-canonical-bytes"),
                                                         reusable_lookup_mode::observe,
                                                         reference_owner(91));
    REQUIRE(other_security_lookup.error == reusable_registry_error::none);
    REQUIRE(other_security_lookup.snapshot.reference == exact_other_security.reference);

    auto candidates = snapshot_reusable_candidates(
        fixture.attachment, candidate_request(reusable_namespace, "coverage-domain"));
    REQUIRE(candidates.has_value());
    REQUIRE(candidates->visited_count == 1);
    REQUIRE(candidates->candidate_count == 1);
    REQUIRE(candidates->candidates[0].generation.reference == reusable.reference);
    REQUIRE(release_reusable_candidate_batch(fixture.attachment, *candidates) ==
            reusable_registry_error::none);
    REQUIRE(release_reusable_reference(fixture.attachment, exact_waiter.token) ==
            reusable_registry_error::none);

    const auto registry = reusable_registry_snapshot(fixture.attachment);
    REQUIRE(registry.indexed_generations == 3);
    REQUIRE(registry.exact_final_generations == 2);
    REQUIRE(registry.reusable_coverage_generations == 1);
    REQUIRE(registry.exact_occupied_buckets == 1);
    REQUIRE(registry.coverage_occupied_buckets == 1);

    std::array<reusable_generation_diagnostics_row, 3> rows{};
    const auto diagnostics = snapshot_reusable_generation_diagnostics(fixture.attachment, rows);
    REQUIRE(diagnostics.error == reusable_registry_error::none);
    REQUIRE(diagnostics.count == 3);
    const auto* exact_row = diagnostics_row(rows, exact.reference);
    const auto* reusable_row = diagnostics_row(rows, reusable.reference);
    const auto* other_security_row = diagnostics_row(rows, exact_other_security.reference);
    REQUIRE(exact_row != nullptr);
    REQUIRE(reusable_row != nullptr);
    REQUIRE(other_security_row != nullptr);
    REQUIRE(exact_row->entry_kind == cache_entry_kind::exact_final_result);
    REQUIRE(reusable_row->entry_kind == cache_entry_kind::reusable_coverage);
    REQUIRE(other_security_row->entry_kind == cache_entry_kind::exact_final_result);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("exact-only retirement unlinks and reclaims without coverage locks",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{1, 4U << 10, 1};
    const auto key = exact_key(23);
    auto claimed = claim_exact(fixture, key, digest(202), "exact-generation", 1);
    REQUIRE(claimed);
    const reusable_generation_reference stale_reference = claimed.reference;
    const reusable_manifest_handle manifest = publish(fixture, stale_reference);
    REQUIRE(static_cast<bool>(manifest));
    REQUIRE(reusable_registry_snapshot(fixture.attachment).coverage_occupied_buckets == 0);

    diagnostics_partition_trace trace{};
    const reusable_registry_partition_callbacks callbacks{
        .context = &trace,
        .acquire_exact = trace_exact_acquire,
        .release_exact = trace_exact_release,
        .acquire_coverage = trace_coverage_acquire,
        .release_coverage = trace_coverage_release,
    };
    const auto retired = retire_reusable_generation(fixture.attachment, stale_reference, callbacks);
    REQUIRE(retired.has_value());
    REQUIRE(*retired == manifest);
    REQUIRE(trace.exact_acquires == 1);
    REQUIRE(trace.exact_releases == 1);
    REQUIRE(trace.coverage_acquires == 0);
    REQUIRE(trace.coverage_releases == 0);
    REQUIRE(reusable_registry_snapshot(fixture.attachment).coverage_occupied_buckets == 0);

    auto absent = lookup_cache_generation(fixture.attachment,
                                          cache_entry_kind::exact_final_result,
                                          key,
                                          std::span<const std::byte>{},
                                          digest(202),
                                          byte_view("exact-generation"),
                                          reusable_lookup_mode::observe,
                                          reference_owner(91));
    REQUIRE(absent.error == reusable_registry_error::not_found);
    REQUIRE(release_and_finalize_one_retired(fixture) == reusable_registry_error::none);

    auto reused = claim_exact(fixture, key, digest(202), "exact-generation", 2);
    REQUIRE(reused);
    REQUIRE(reused.reference.generation_slot == stale_reference.generation_slot);
    REQUIRE(reused.reference.slot_sequence != stale_reference.slot_sequence);
    auto stale = snapshot_reusable_generation(fixture.attachment, stale_reference);
    REQUIRE_FALSE(stale.has_value());
    REQUIRE(stale.error() == reusable_registry_error::stale_reference);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("exact claims keep dependency versions distinct",
          "[component:cache][layer:unit][feature:registry]") {
    SECTION("in-flight work also binds the snapshot completion handshake") {
        registry_fixture fixture{4, 16U << 10, 1};
        const auto key = exact_key(224);
        const auto generation_digest = digest(224);
        const std::string_view generation = "distinct-in-flight";
        const reusable_conservative_guard first_guard = conservative_guard();

        auto first =
            claim_exact_with_guard(fixture, key, generation_digest, generation, 1, first_guard);
        REQUIRE(first);
        REQUIRE(first.disposition == reusable_claim_disposition::claimed_producer);

        reusable_conservative_guard later_snapshot = first_guard;
        ++later_snapshot.snapshot_completion_count;
        auto second =
            claim_exact_with_guard(fixture, key, generation_digest, generation, 2, later_snapshot);
        REQUIRE(second);
        REQUIRE(second.disposition == reusable_claim_disposition::claimed_producer);
        REQUIRE(second.reference != first.reference);

        auto coalesced =
            claim_exact_with_guard(fixture, key, generation_digest, generation, 3, first_guard);
        REQUIRE(coalesced);
        REQUIRE(coalesced.disposition == reusable_claim_disposition::existing_in_flight);
        REQUIRE(coalesced.reference == first.reference);
        REQUIRE(coalesced.token.active);
        REQUIRE(release_reusable_reference(fixture.attachment, coalesced.token) ==
                reusable_registry_error::none);
        REQUIRE(reusable_registry_snapshot(fixture.attachment).indexed_generations == 2);
        REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
    }

    SECTION("complete hits ignore rollback noise but reject a newer guard") {
        registry_fixture fixture{4, 16U << 10, 1};
        const auto key = exact_key(225);
        const auto generation_digest = digest(225);
        const std::string_view generation = "distinct-complete";
        const reusable_conservative_guard first_guard = conservative_guard();

        auto first =
            claim_exact_with_guard(fixture, key, generation_digest, generation, 1, first_guard);
        REQUIRE(first);
        REQUIRE(static_cast<bool>(publish(fixture, first.reference)));

        reusable_conservative_guard rollback_only = first_guard;
        ++rollback_only.snapshot_completion_count;
        auto hit = claim_exact_with_guard(
            fixture, key, generation_digest, generation, 2, rollback_only, 300);
        REQUIRE(hit);
        REQUIRE(hit.disposition == reusable_claim_disposition::existing_complete);
        REQUIRE(hit.reference == first.reference);
        REQUIRE(hit.token.active);
        REQUIRE(release_reusable_reference(fixture.attachment, hit.token) ==
                reusable_registry_error::none);

        reusable_conservative_guard committed_write = rollback_only;
        ++committed_write.dependency.version.committed_epoch;
        ++committed_write.dependency.record_generation;
        auto newer = claim_exact_with_guard(
            fixture, key, generation_digest, generation, 3, committed_write, 400);
        REQUIRE(newer);
        REQUIRE(newer.disposition == reusable_claim_disposition::claimed_producer);
        REQUIRE(newer.reference != first.reference);

        const auto old_snapshot = snapshot_reusable_generation(fixture.attachment, first.reference);
        REQUIRE(old_snapshot.has_value());
        REQUIRE(old_snapshot->state == cache_entry_state::complete);
        REQUIRE(old_snapshot->metadata.dependency_count == k_test_dependency_count);
        REQUIRE(old_snapshot->conservative_guard == first_guard);
        REQUIRE(reusable_registry_snapshot(fixture.attachment).indexed_generations == 2);
        REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
    }
}

TEST_CASE("reserved guard kind follows generation observability",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{4, 16U << 10, 1};
    const test_dependency_envelope dependencies = dependency_envelope();
    reusable_claim_request precise_request{
        .entry_kind = cache_entry_kind::exact_final_result,
        .coverage_directory_key = exact_key(229),
        .generation_digest = digest(229),
        .canonical_generation_key = byte_view("definition-guard"),
        .dependency_vector_envelope = dependencies,
        .conservative_guard = definition_guard(),
        .metadata = metadata(),
        .producer = producer(1),
        .reference_owner = reference_owner(1),
        .started_monotonic_ns = 100,
    };
    const reusable_claim_result precise =
        claim_reusable_generation(fixture.attachment, precise_request);
    REQUIRE(precise);
    REQUIRE(precise.disposition == reusable_claim_disposition::claimed_producer);

    auto precise_with_write = precise_request;
    precise_with_write.generation_digest = digest(230);
    precise_with_write.canonical_generation_key = byte_view("wrong-write");
    precise_with_write.conservative_guard = conservative_guard();
    REQUIRE(claim_reusable_generation(fixture.attachment, precise_with_write).error ==
            reusable_registry_error::invalid_argument);

    auto conservative_with_definition = precise_request;
    conservative_with_definition.generation_digest = digest(231);
    conservative_with_definition.canonical_generation_key = byte_view("wrong-definition");
    conservative_with_definition.metadata = exact_metadata();
    REQUIRE(claim_reusable_generation(fixture.attachment, conservative_with_definition).error ==
            reusable_registry_error::invalid_argument);

    auto definition_with_completion = precise_request;
    definition_with_completion.generation_digest = digest(232);
    definition_with_completion.canonical_generation_key = byte_view("definition-with-completion");
    definition_with_completion.conservative_guard.snapshot_completion_count = 1;
    REQUIRE(claim_reusable_generation(fixture.attachment, definition_with_completion).error ==
            reusable_registry_error::invalid_argument);

    auto write_without_completion = conservative_guard();
    write_without_completion.snapshot_completion_count = 0;
    auto conservative_without_completion = conservative_with_definition;
    conservative_without_completion.generation_digest = digest(233);
    conservative_without_completion.canonical_generation_key =
        byte_view("write-without-completion");
    conservative_without_completion.conservative_guard = write_without_completion;
    REQUIRE(claim_reusable_generation(fixture.attachment, conservative_without_completion).error ==
            reusable_registry_error::invalid_argument);
}

TEST_CASE("dependency release retries an exact applied prefix",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{1, 4U << 10, 1};
    const auto key = exact_key(230);
    auto claimed = claim_exact(fixture, key, digest(230), "release-prefix", 1);
    REQUIRE(claimed);
    const reusable_manifest_handle manifest = publish(fixture, claimed.reference);
    REQUIRE(static_cast<bool>(manifest));
    REQUIRE(retire_reusable_generation(fixture.attachment, claimed.reference).has_value());

    reusable_shared_generation& record = shared_generation(fixture, claimed.reference);
    REQUIRE(record.dependency_release_state == reusable_dependency_release_state::pending);
    REQUIRE(record.dependency_release_cursor == 0);
    REQUIRE(record.dependency_release_sequence == 0);

    std::array<std::byte, 1> short_copy{};
    const reusable_dependency_release_preparation short_preparation =
        prepare_reusable_dependency_release(fixture.attachment, short_copy);
    REQUIRE(short_preparation.error == reusable_registry_error::region_too_small);
    REQUIRE(short_preparation.required_vector_bytes ==
            dependency_vector_envelope_bytes(k_test_dependency_count));
    REQUIRE(short_preparation.copied_vector_bytes == 0);
    REQUIRE_FALSE(short_preparation.ticket);
    REQUIRE(record.dependency_release_state == reusable_dependency_release_state::pending);
    REQUIRE(record.dependency_release_cursor == 0);
    REQUIRE(record.dependency_release_sequence == 0);

    test_dependency_envelope first_copy{};
    const reusable_dependency_release_preparation first =
        prepare_reusable_dependency_release(fixture.attachment, first_copy);
    REQUIRE(first);
    REQUIRE(first.ticket.first_reference == 0);
    REQUIRE(first.ticket.dependency_count == k_test_dependency_count);
    REQUIRE(first.ticket.reference_count == k_test_dependency_count + 1);
    REQUIRE(first.ticket.conservative_guard == conservative_guard());
    const auto partial = record_reusable_dependency_release_progress(
        fixture.attachment, first.ticket, k_test_dependency_count);
    REQUIRE(partial.has_value());
    REQUIRE_FALSE(*partial);
    REQUIRE(record.dependency_release_state == reusable_dependency_release_state::pending);
    REQUIRE(record.dependency_release_cursor == k_test_dependency_count);
    REQUIRE(record.dependency_release_sequence == 0);

    const auto stale_partial = record_reusable_dependency_release_progress(
        fixture.attachment, first.ticket, k_test_dependency_count);
    REQUIRE_FALSE(stale_partial.has_value());
    REQUIRE(stale_partial.error() == reusable_registry_error::stale_reference);

    test_dependency_envelope second_copy{};
    const reusable_dependency_release_preparation second =
        prepare_reusable_dependency_release(fixture.attachment, second_copy);
    REQUIRE(second);
    REQUIRE(second.ticket.release_sequence != first.ticket.release_sequence);
    REQUIRE(second.ticket.first_reference == k_test_dependency_count);
    REQUIRE(second.ticket.reference_count == k_test_dependency_count + 1);
    const auto complete =
        record_reusable_dependency_release_progress(fixture.attachment, second.ticket, 1);
    REQUIRE(complete.has_value());
    REQUIRE(*complete);
    REQUIRE(record.dependency_release_state == reusable_dependency_release_state::released);
    REQUIRE(record.dependency_release_cursor == k_test_dependency_count + 1);

    const auto duplicate =
        record_reusable_dependency_release_progress(fixture.attachment, second.ticket, 1);
    REQUIRE(duplicate.has_value());
    REQUIRE(*duplicate);
    const auto finalized = finalize_reusable_generation_reclaim(fixture.attachment, second.ticket);
    REQUIRE(finalized.has_value());
    REQUIRE(*finalized == manifest);

    auto reused = claim_exact(fixture, key, digest(230), "release-prefix", 2);
    REQUIRE(reused);
    REQUIRE(reused.reference.generation_slot == claimed.reference.generation_slot);
    REQUIRE(reused.reference.slot_sequence != claimed.reference.slot_sequence);
    const auto stale_after_reuse =
        record_reusable_dependency_release_progress(fixture.attachment, second.ticket, 1);
    REQUIRE_FALSE(stale_after_reuse.has_value());
    REQUIRE(stale_after_reuse.error() == reusable_registry_error::stale_reference);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("dependency release finalizes an arbitrary retired-list position",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{2, 8U << 10, 1};
    auto first_claim = claim_exact(fixture, exact_key(231), digest(231), "retired-first", 1);
    REQUIRE(first_claim);
    const reusable_manifest_handle first_manifest = publish(fixture, first_claim.reference);
    REQUIRE(static_cast<bool>(first_manifest));
    REQUIRE(retire_reusable_generation(fixture.attachment, first_claim.reference).has_value());

    test_dependency_envelope copied{};
    const reusable_dependency_release_preparation prepared =
        prepare_reusable_dependency_release(fixture.attachment, copied);
    REQUIRE(prepared);
    REQUIRE(prepared.ticket.reference == first_claim.reference);

    reusable_dependency_release_ticket forged = prepared.ticket;
    ++forged.reference.slot_sequence;
    const auto rejected = record_reusable_dependency_release_progress(
        fixture.attachment, forged, k_test_dependency_count);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error() == reusable_registry_error::stale_reference);

    auto second_claim = claim_exact(fixture, exact_key(232), digest(232), "retired-second", 2);
    REQUIRE(second_claim);
    REQUIRE(static_cast<bool>(publish(fixture, second_claim.reference)));
    REQUIRE(retire_reusable_generation(fixture.attachment, second_claim.reference).has_value());

    const auto complete = record_reusable_dependency_release_progress(
        fixture.attachment, prepared.ticket, prepared.ticket.reference_count);
    REQUIRE(complete.has_value());
    REQUIRE(*complete);
    const auto finalized =
        finalize_reusable_generation_reclaim(fixture.attachment, prepared.ticket);
    REQUIRE(finalized.has_value());
    REQUIRE(*finalized == first_manifest);
    REQUIRE(reusable_registry_snapshot(fixture.attachment).retired_generations == 1);
    REQUIRE(release_and_finalize_one_retired(fixture) == reusable_registry_error::none);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("dependency release rejects a corrupt retained envelope",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{1, 4U << 10, 1};
    auto claimed = claim_exact(fixture, exact_key(233), digest(233), "corrupt-release-vector", 1);
    REQUIRE(claimed);
    REQUIRE(static_cast<bool>(publish(fixture, claimed.reference)));
    REQUIRE(retire_reusable_generation(fixture.attachment, claimed.reference).has_value());

    reusable_shared_generation& record = shared_generation(fixture, claimed.reference);
    REQUIRE(record.dependency_release_state == reusable_dependency_release_state::pending);
    REQUIRE(record.dependency_release_sequence == 0);
    fixture.attachment.region[record.dependency_vector.offset] ^= std::byte{1};

    test_dependency_envelope copied{};
    const reusable_dependency_release_preparation prepared =
        prepare_reusable_dependency_release(fixture.attachment, copied);
    REQUIRE(prepared.error == reusable_registry_error::corrupt_generation);
    REQUIRE_FALSE(prepared.ticket);
    REQUIRE(record.dependency_release_state == reusable_dependency_release_state::pending);
    REQUIRE(record.dependency_release_cursor == 0);
    REQUIRE(record.dependency_release_sequence == 0);
    REQUIRE(validate_reusable_registry(fixture.attachment) ==
            reusable_registry_error::corrupt_generation);
}

TEST_CASE("dependency release rejects a corrupt conservative guard",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{1, 4U << 10, 1};
    auto claimed = claim_exact(fixture, exact_key(234), digest(234), "corrupt-release-guard", 1);
    REQUIRE(claimed);
    REQUIRE(static_cast<bool>(publish(fixture, claimed.reference)));
    REQUIRE(retire_reusable_generation(fixture.attachment, claimed.reference).has_value());

    reusable_shared_generation& record = shared_generation(fixture, claimed.reference);
    REQUIRE(record.conservative_guard.active);
    REQUIRE(record.metadata.dependency_count == k_test_dependency_count);
    record.conservative_guard.dependency.record_generation = 0;

    test_dependency_envelope copied{};
    const reusable_dependency_release_preparation prepared =
        prepare_reusable_dependency_release(fixture.attachment, copied);
    REQUIRE(prepared.error == reusable_registry_error::corrupt_generation);
    REQUIRE_FALSE(prepared.ticket);
    REQUIRE(record.dependency_release_state == reusable_dependency_release_state::pending);
    REQUIRE(record.dependency_release_cursor == 0);
    REQUIRE(record.dependency_release_sequence == 0);
    REQUIRE(validate_reusable_registry(fixture.attachment) ==
            reusable_registry_error::corrupt_generation);
}

TEST_CASE("claim kind and kind-specific identity shapes are validated",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{4, 16U << 10, 1};
    const auto key = exact_key(24);
    const test_dependency_envelope dependencies = dependency_envelope();
    reusable_claim_request exact_request{
        .entry_kind = cache_entry_kind::exact_final_result,
        .coverage_directory_key = key,
        .generation_digest = digest(203),
        .canonical_generation_key = byte_view("exact"),
        .dependency_vector_envelope = dependencies,
        .conservative_guard = conservative_guard(),
        .metadata = exact_metadata(),
        .producer = producer(1),
        .reference_owner = reference_owner(1),
        .started_monotonic_ns = 100,
    };

    auto invalid_kind = exact_request;
    invalid_kind.entry_kind = static_cast<cache_entry_kind>(0);
    REQUIRE(claim_reusable_generation(fixture.attachment, invalid_kind).error ==
            reusable_registry_error::invalid_argument);
    invalid_kind.entry_kind = static_cast<cache_entry_kind>(3);
    REQUIRE(claim_reusable_generation(fixture.attachment, invalid_kind).error ==
            reusable_registry_error::invalid_argument);

    auto exact_with_domain = exact_request;
    exact_with_domain.canonical_domain_key = byte_view("not-exact");
    REQUIRE(claim_reusable_generation(fixture.attachment, exact_with_domain).error ==
            reusable_registry_error::invalid_argument);
    auto exact_with_descriptor = exact_request;
    exact_with_descriptor.coverage_descriptor = byte_view("descriptor");
    REQUIRE(claim_reusable_generation(fixture.attachment, exact_with_descriptor).error ==
            reusable_registry_error::invalid_argument);
    auto exact_with_coverage_digest = exact_request;
    exact_with_coverage_digest.coverage_directory_key.coverage_domain_digest = digest(204);
    REQUIRE(claim_reusable_generation(fixture.attachment, exact_with_coverage_digest).error ==
            reusable_registry_error::invalid_argument);

    auto invalid_lookup = lookup_cache_generation(fixture.attachment,
                                                  static_cast<cache_entry_kind>(99),
                                                  key,
                                                  std::span<const std::byte>{},
                                                  digest(203),
                                                  byte_view("exact"),
                                                  reusable_lookup_mode::observe,
                                                  reference_owner(2));
    REQUIRE(invalid_lookup.error == reusable_registry_error::invalid_argument);

    auto claimed = claim_reusable_generation(fixture.attachment, exact_request);
    REQUIRE(claimed);
    auto* records = reinterpret_cast<reusable_shared_generation*>(
        fixture.attachment.region.data() + fixture.request_.layout.generations_offset);
    reusable_shared_generation& record = records[claimed.reference.generation_slot];
    record.entry_kind = static_cast<cache_entry_kind>(99);
    REQUIRE(validate_reusable_registry(fixture.attachment) ==
            reusable_registry_error::corrupt_generation);
    record.entry_kind = cache_entry_kind::exact_final_result;
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("coverage lookup proves full domain bytes after digest match",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{8};
    const auto key = coverage_key(23, 70);
    auto left = claim(fixture, key, digest(10), "domain-left", "left-generation", 1);
    auto right = claim(fixture, key, digest(11), "domain-right", "right-generation", 2);
    REQUIRE(left);
    REQUIRE(right);

    auto candidates =
        snapshot_reusable_candidates(fixture.attachment, candidate_request(key, "domain-left"));
    REQUIRE(candidates.has_value());
    REQUIRE(candidates->visited_count == 2);
    REQUIRE(candidates->domain_collision_count == 1);
    REQUIRE(candidates->candidate_count == 1);
    REQUIRE(candidates->candidates[0].generation.reference == left.reference);
    REQUIRE(release_reusable_candidate_batch(fixture.attachment, *candidates) ==
            reusable_registry_error::none);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("security namespaces own independent bounded 64 member domains",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{66, 1U << 20, 1};
    const auto crowded_key = coverage_key(30, 80);
    std::vector<reusable_generation_reference> references;
    references.reserve(k_maximum_coverage_generations_per_domain);
    for (std::uint32_t index = 0; index < k_maximum_coverage_generations_per_domain; ++index) {
        const std::string generation = "generation-" + std::to_string(index);
        auto claimed = claim(fixture,
                             crowded_key,
                             digest(static_cast<std::uint8_t>(index)),
                             "crowded-domain",
                             generation,
                             index + 1U);
        REQUIRE(claimed.disposition == reusable_claim_disposition::claimed_producer);
        references.push_back(claimed.reference);
    }

    auto overflow = claim(fixture, crowded_key, digest(200), "crowded-domain", "overflow", 100);
    REQUIRE(overflow.error == reusable_registry_error::coverage_bucket_full);

    const auto other_namespace = coverage_key(31, 80);
    auto independent =
        claim(fixture, other_namespace, digest(201), "crowded-domain", "independent", 101);
    REQUIRE(independent.disposition == reusable_claim_disposition::claimed_producer);

    auto candidates = snapshot_reusable_candidates(
        fixture.attachment, candidate_request(crowded_key, "crowded-domain"));
    REQUIRE(candidates.has_value());
    REQUIRE(candidates->candidate_count == k_maximum_coverage_generations_per_domain);
    REQUIRE(candidates->selection_truncated);
    for (std::size_t index = 1; index < candidates->candidate_count; ++index) {
        REQUIRE(candidates->candidates[index - 1U].generation.reference.id.claim_sequence <
                candidates->candidates[index].generation.reference.id.claim_sequence);
    }
    REQUIRE(release_reusable_candidate_batch(fixture.attachment, *candidates) ==
            reusable_registry_error::none);

    auto isolated = snapshot_reusable_candidates(
        fixture.attachment, candidate_request(other_namespace, "crowded-domain"));
    REQUIRE(isolated.has_value());
    REQUIRE(isolated->visited_count == 1);
    REQUIRE(isolated->candidate_count == 1);
    REQUIRE(isolated->candidates[0].generation.reference == independent.reference);
    REQUIRE(release_reusable_candidate_batch(fixture.attachment, *isolated) ==
            reusable_registry_error::none);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("exact ninth claim retires the least recently used terminal member",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{12, 1U << 20, 1};
    const auto key = exact_key(32);
    const cache_digest_256 shared_digest = digest(220);
    std::array<reusable_generation_reference, k_maximum_generations_per_exact_digest> references{};
    std::array<reusable_manifest_handle, k_maximum_generations_per_exact_digest> manifests{};
    constexpr std::size_t expected_victim = 5;

    for (std::size_t index = 0; index < references.size(); ++index) {
        const std::string generation = "exact-saturated-" + std::to_string(index);
        auto claimed = claim_exact(fixture,
                                   key,
                                   shared_digest,
                                   generation,
                                   static_cast<std::uint32_t>(index + 1U),
                                   static_cast<std::uint64_t>(index + 1U));
        REQUIRE(claimed);
        references[index] = claimed.reference;
        const std::uint64_t completed_ns = index == expected_victim ? 100U : 500U + index;
        manifests[index] = publish(fixture, claimed.reference, completed_ns);
        REQUIRE(static_cast<bool>(manifests[index]));
    }

    auto ninth = claim_exact(fixture, key, shared_digest, "exact-saturated-new", 20, 1'000);
    REQUIRE(ninth);
    REQUIRE(ninth.disposition == reusable_claim_disposition::claimed_producer);
    REQUIRE(ninth.retired_manifest_count == 1);
    REQUIRE(ninth.retired_manifests[0] == manifests[expected_victim]);

    const auto snapshot = reusable_registry_snapshot(fixture.attachment);
    REQUIRE(snapshot.indexed_generations == k_maximum_generations_per_exact_digest);
    REQUIRE(snapshot.retired_generations == 1);
    REQUIRE(snapshot.evictions == 1);
    REQUIRE_FALSE(
        snapshot_reusable_generation(fixture.attachment, references[expected_victim]).has_value());
    REQUIRE(snapshot_reusable_generation(fixture.attachment, references[0]).has_value());
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);

    REQUIRE(release_and_finalize_one_retired(fixture) == reusable_registry_error::none);
    REQUIRE(reusable_registry_snapshot(fixture.attachment).retired_generations == 0);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("saturated exact bucket declines when every terminal member is pinned",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{10, 1U << 20, 1};
    const auto key = exact_key(33);
    const cache_digest_256 shared_digest = digest(221);
    std::array<reusable_reference_token, k_maximum_generations_per_exact_digest> pins{};

    for (std::size_t index = 0; index < pins.size(); ++index) {
        const std::string generation = "exact-pinned-" + std::to_string(index);
        auto claimed = claim_exact(fixture,
                                   key,
                                   shared_digest,
                                   generation,
                                   static_cast<std::uint32_t>(index + 1U),
                                   static_cast<std::uint64_t>(index + 1U));
        REQUIRE(claimed);
        REQUIRE(static_cast<bool>(publish(fixture, claimed.reference, 200U + index)));
        auto pin =
            pin_reusable_generation(fixture.attachment,
                                    claimed.reference,
                                    reusable_reference_kind::reader_pin,
                                    reference_owner(static_cast<std::uint32_t>(100U + index)));
        REQUIRE(pin.has_value());
        pins[index] = *pin;
    }

    auto declined = claim_exact(fixture, key, shared_digest, "exact-pinned-overflow", 30, 1'000);
    REQUIRE_FALSE(declined);
    REQUIRE(declined.error == reusable_registry_error::exact_bucket_full);
    REQUIRE(declined.retired_manifest_count == 0);
    REQUIRE(reusable_registry_snapshot(fixture.attachment).indexed_generations ==
            k_maximum_generations_per_exact_digest);
    REQUIRE(reusable_registry_snapshot(fixture.attachment).retired_generations == 0);

    for (reusable_reference_token& pin : pins) {
        REQUIRE(release_reusable_reference(fixture.attachment, pin) ==
                reusable_registry_error::none);
    }
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("saturated admission allocation failure does not retire a victim",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{10, 2U << 10, 1};
    const auto key = exact_key(37);
    const cache_digest_256 shared_digest = digest(222);

    for (std::size_t index = 0; index < k_maximum_generations_per_exact_digest; ++index) {
        const std::string generation = "g" + std::to_string(index);
        auto claimed = claim_exact(fixture,
                                   key,
                                   shared_digest,
                                   generation,
                                   static_cast<std::uint32_t>(index + 1U),
                                   index + 1U);
        REQUIRE(claimed);
        REQUIRE(static_cast<bool>(publish(fixture, claimed.reference, 200U + index)));
    }

    const std::string oversized_for_remaining_arena(300, 'x');
    auto denied =
        claim_exact(fixture, key, shared_digest, oversized_for_remaining_arena, 50, 1'000);
    REQUIRE_FALSE(denied);
    REQUIRE(denied.error == reusable_registry_error::byte_arena_full);
    REQUIRE(denied.retired_manifest_count == 0);
    REQUIRE(reusable_registry_snapshot(fixture.attachment).indexed_generations ==
            k_maximum_generations_per_exact_digest);
    REQUIRE(reusable_registry_snapshot(fixture.attachment).retired_generations == 0);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("coverage sixty fifth claim retires one LRU member from both indexes",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{68, 2U << 20, 1};
    const auto key = coverage_key(34, 82);
    constexpr std::string_view domain = "coverage-saturated-domain";
    constexpr std::size_t expected_victim = 17;
    std::array<reusable_generation_reference, k_maximum_coverage_generations_per_domain>
        references{};
    std::array<reusable_manifest_handle, k_maximum_coverage_generations_per_domain> manifests{};
    std::array<std::string, k_maximum_coverage_generations_per_domain> generation_keys{};

    for (std::size_t index = 0; index < references.size(); ++index) {
        generation_keys[index] = "coverage-saturated-" + std::to_string(index);
        auto claimed = claim(fixture,
                             key,
                             digest(static_cast<std::uint8_t>(index)),
                             domain,
                             generation_keys[index],
                             static_cast<std::uint32_t>(index + 1U),
                             static_cast<std::uint64_t>(index + 1U));
        REQUIRE(claimed);
        references[index] = claimed.reference;
        const std::uint64_t completed_ns = index == expected_victim ? 100U : 500U + index;
        manifests[index] = publish(fixture, claimed.reference, completed_ns);
        REQUIRE(static_cast<bool>(manifests[index]));
    }

    auto sixty_fifth =
        claim(fixture, key, digest(200), domain, "coverage-saturated-new", 100, 2'000);
    REQUIRE(sixty_fifth);
    REQUIRE(sixty_fifth.retired_manifest_count == 1);
    REQUIRE(sixty_fifth.retired_manifests[0] == manifests[expected_victim]);

    auto absent = lookup_reusable_generation(fixture.attachment,
                                             key,
                                             byte_view(domain),
                                             digest(static_cast<std::uint8_t>(expected_victim)),
                                             byte_view(generation_keys[expected_victim]),
                                             reusable_lookup_mode::observe,
                                             reference_owner(201));
    REQUIRE(absent.error == reusable_registry_error::not_found);
    auto present = lookup_reusable_generation(fixture.attachment,
                                              key,
                                              byte_view(domain),
                                              digest(200),
                                              byte_view("coverage-saturated-new"),
                                              reusable_lookup_mode::observe,
                                              reference_owner(202));
    REQUIRE(present.error == reusable_registry_error::none);

    auto candidates =
        snapshot_reusable_candidates(fixture.attachment, candidate_request(key, domain));
    REQUIRE(candidates.has_value());
    REQUIRE(candidates->visited_count == k_maximum_coverage_generations_per_domain);
    REQUIRE(candidates->candidate_count == k_maximum_coverage_generations_per_domain);
    REQUIRE(release_reusable_candidate_batch(fixture.attachment, *candidates) ==
            reusable_registry_error::none);
    REQUIRE(reusable_registry_snapshot(fixture.attachment).indexed_generations ==
            k_maximum_coverage_generations_per_domain);
    REQUIRE(reusable_registry_snapshot(fixture.attachment).retired_generations == 1);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);

    REQUIRE(release_and_finalize_one_retired(fixture) == reusable_registry_error::none);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("independently saturated exact and coverage buckets report both manifests",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{78, 2U << 20, 1};
    const cache_digest_256 shared_digest = digest(250);
    const auto exact_namespace = exact_key(35);
    const auto coverage_namespace = coverage_key(36, 83);
    constexpr std::string_view domain = "dual-saturated-domain";
    reusable_manifest_handle exact_victim_manifest{};
    reusable_generation_reference exact_victim_reference{};
    reusable_manifest_handle coverage_victim_manifest{};
    reusable_generation_reference coverage_victim_reference{};

    for (std::size_t index = 0; index < k_maximum_generations_per_exact_digest; ++index) {
        const std::string generation = "dual-exact-" + std::to_string(index);
        auto claimed = claim_exact(fixture,
                                   exact_namespace,
                                   shared_digest,
                                   generation,
                                   static_cast<std::uint32_t>(index + 1U),
                                   index + 1U);
        REQUIRE(claimed);
        const reusable_manifest_handle manifest = publish(fixture, claimed.reference, 300U + index);
        REQUIRE(static_cast<bool>(manifest));
        if (index == 0) {
            exact_victim_manifest = manifest;
            exact_victim_reference = claimed.reference;
        }
    }
    for (std::size_t index = 0; index < k_maximum_coverage_generations_per_domain; ++index) {
        const std::string generation = "dual-coverage-" + std::to_string(index);
        auto claimed = claim(fixture,
                             coverage_namespace,
                             digest(static_cast<std::uint8_t>(index)),
                             domain,
                             generation,
                             static_cast<std::uint32_t>(100U + index),
                             100U + index);
        REQUIRE(claimed);
        const reusable_manifest_handle manifest = publish(fixture, claimed.reference, 500U + index);
        REQUIRE(static_cast<bool>(manifest));
        if (index == 0) {
            coverage_victim_manifest = manifest;
            coverage_victim_reference = claimed.reference;
        }
    }

    auto admitted =
        claim(fixture, coverage_namespace, shared_digest, domain, "dual-saturated-new", 500, 2'000);
    REQUIRE(admitted);
    REQUIRE(admitted.retired_manifest_count == 2);
    REQUIRE(admitted.retired_manifests[0] == exact_victim_manifest);
    REQUIRE(admitted.retired_manifests[1] == coverage_victim_manifest);
    REQUIRE_FALSE(
        snapshot_reusable_generation(fixture.attachment, exact_victim_reference).has_value());
    REQUIRE_FALSE(
        snapshot_reusable_generation(fixture.attachment, coverage_victim_reference).has_value());
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);

    REQUIRE(release_and_finalize_one_retired(fixture) == reusable_registry_error::none);
    REQUIRE(release_and_finalize_one_retired(fixture) == reusable_registry_error::none);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("every reusable security field partitions discovery",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{8, 1U << 20, 1};
    const auto base = coverage_key(33, 81);
    std::array<reusable_coverage_directory_key, 7> keys{};
    keys.fill(base);
    ++keys[1].security_namespace.database_oid;
    ++keys[2].security_namespace.current_role_oid;
    ++keys[3].security_namespace.session_user_oid;
    ++keys[4].security_namespace.security_context_flags;
    keys[5].security_namespace.row_security_mode = 0;
    keys[6].security_namespace.provider_security_digest = k_provider_independent_security_digest;

    std::array<reusable_generation_reference, 7> references{};
    for (std::size_t index = 0; index < keys.size(); ++index) {
        const std::string generation = "security-generation-" + std::to_string(index);
        auto claimed = claim(fixture,
                             keys[index],
                             digest(static_cast<std::uint8_t>(120 + index)),
                             "one-domain",
                             generation,
                             static_cast<std::uint32_t>(index + 1));
        REQUIRE(claimed.disposition == reusable_claim_disposition::claimed_producer);
        references[index] = claimed.reference;
    }

    for (std::size_t index = 0; index < keys.size(); ++index) {
        auto candidates = snapshot_reusable_candidates(
            fixture.attachment, candidate_request(keys[index], "one-domain"));
        REQUIRE(candidates.has_value());
        REQUIRE(candidates->visited_count == 1);
        REQUIRE(candidates->candidate_count == 1);
        REQUIRE(candidates->candidates[0].generation.reference == references[index]);
        REQUIRE(release_reusable_candidate_batch(fixture.attachment, *candidates) ==
                reusable_registry_error::none);
    }
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("reusable security namespace rejects invalid sentinels",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{2};
    const auto valid = coverage_key(34, 82);
    auto require_rejected = [&fixture](reusable_coverage_directory_key key) {
        const auto result = claim(fixture, key, digest(140), "domain", "generation", 1);
        REQUIRE(result.error == reusable_registry_error::invalid_argument);
        REQUIRE(result.disposition == reusable_claim_disposition::rejected);
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
        k_provider_independent_security_digest;
    REQUIRE(claim(fixture, provider_independent, digest(141), "domain", "provider-independent", 2)
                .disposition == reusable_claim_disposition::claimed_producer);
}

TEST_CASE("two index retirement and reclaim reject stale ABA references",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{1, 4U << 10, 1};
    const auto key = coverage_key(40);
    auto claimed = claim(fixture, key, digest(40), "domain", "generation", 1);
    REQUIRE(claimed);
    const auto old_reference = claimed.reference;
    const auto manifest = publish(fixture, old_reference);
    REQUIRE(static_cast<bool>(manifest));

    auto planner_pin = pin_reusable_generation(fixture.attachment,
                                               old_reference,
                                               reusable_reference_kind::planner_pin,
                                               reference_owner(91));
    REQUIRE(planner_pin.has_value());
    auto pinned_retire = retire_reusable_generation(fixture.attachment, old_reference);
    REQUIRE_FALSE(pinned_retire.has_value());
    REQUIRE(pinned_retire.error() == reusable_registry_error::invalid_state);
    REQUIRE(release_reusable_reference(fixture.attachment, *planner_pin) ==
            reusable_registry_error::none);

    auto retired = retire_reusable_generation(fixture.attachment, old_reference);
    REQUIRE(retired.has_value());
    REQUIRE(retired->id == manifest.id);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);

    auto exact = lookup_reusable_generation(fixture.attachment,
                                            key,
                                            byte_view("domain"),
                                            digest(40),
                                            byte_view("generation"),
                                            reusable_lookup_mode::observe,
                                            reference_owner(92));
    REQUIRE(exact.error == reusable_registry_error::not_found);
    auto coverage =
        snapshot_reusable_candidates(fixture.attachment, candidate_request(key, "domain"));
    REQUIRE(coverage.has_value());
    REQUIRE(coverage->candidate_count == 0);

    REQUIRE(release_and_finalize_one_retired(fixture) == reusable_registry_error::none);
    auto reused = claim(fixture, key, digest(40), "domain", "generation", 2);
    REQUIRE(reused);
    REQUIRE(reused.reference.generation_slot == old_reference.generation_slot);
    REQUIRE(reused.reference.slot_sequence != old_reference.slot_sequence);
    auto stale = snapshot_reusable_generation(fixture.attachment, old_reference);
    REQUIRE_FALSE(stale.has_value());
    REQUIRE(stale.error() == reusable_registry_error::stale_reference);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("reusable reference owners are ABA safe and bounded",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{2, 16U << 10, 1};
    const auto key = coverage_key(45);
    auto claimed = claim(fixture, key, digest(45), "domain", "generation", 1);
    REQUIRE(claimed);

    std::vector<reusable_reference_token> pins;
    pins.reserve(k_reusable_reference_owners_per_generation);
    for (std::uint32_t index = 0; index < k_reusable_reference_owners_per_generation; ++index) {
        auto pin = pin_reusable_generation(fixture.attachment,
                                           claimed.reference,
                                           reusable_reference_kind::planner_pin,
                                           reference_owner(200 + index));
        REQUIRE(pin.has_value());
        pins.push_back(*pin);
    }
    auto overflow = pin_reusable_generation(fixture.attachment,
                                            claimed.reference,
                                            reusable_reference_kind::planner_pin,
                                            reference_owner(300));
    REQUIRE_FALSE(overflow.has_value());
    REQUIRE(overflow.error() == reusable_registry_error::reference_slots_full);

    for (reusable_reference_token& pin : pins) {
        REQUIRE(release_reusable_reference(fixture.attachment, pin) ==
                reusable_registry_error::none);
    }
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("janitor owner snapshots are bounded and ABA-token complete",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{5, 64U << 10, 1};

    const auto exact_namespace = exact_key(45);
    auto exact = claim_exact(fixture, exact_namespace, digest(205), "exact-owner-generation", 1);
    REQUIRE(exact);
    auto exact_waiter =
        claim_exact(fixture, exact_namespace, digest(205), "exact-owner-generation", 2);
    REQUIRE(exact_waiter.token.active);
    auto exact_planner = pin_reusable_generation(fixture.attachment,
                                                 exact.reference,
                                                 reusable_reference_kind::planner_pin,
                                                 reference_owner(450));
    REQUIRE(exact_planner.has_value());

    const auto reusable_namespace = coverage_key(45, 205);
    auto reusable = claim(
        fixture, reusable_namespace, digest(206), "owner-domain", "coverage-owner-generation", 3);
    REQUIRE(reusable);
    REQUIRE(static_cast<bool>(publish(fixture, reusable.reference)));
    auto reusable_planner = pin_reusable_generation(fixture.attachment,
                                                    reusable.reference,
                                                    reusable_reference_kind::planner_pin,
                                                    reference_owner(451));
    REQUIRE(reusable_planner.has_value());
    auto reusable_reader = lookup_reusable_generation(fixture.attachment,
                                                      reusable_namespace,
                                                      byte_view("owner-domain"),
                                                      digest(206),
                                                      byte_view("coverage-owner-generation"),
                                                      reusable_lookup_mode::pin_complete,
                                                      reference_owner(452));
    REQUIRE(reusable_reader.error == reusable_registry_error::none);

    std::array<reusable_janitor_owner_observation, 8> observations{};
    const auto snapshot = snapshot_reusable_janitor_liveness(fixture.attachment, observations, 5);
    REQUIRE(snapshot.error == reusable_registry_error::none);
    REQUIRE_FALSE(snapshot.truncated);
    REQUIRE(snapshot.scan_start == 0);
    REQUIRE(snapshot.scanned_generations == 5);
    REQUIRE(snapshot.count == 5);
    std::size_t producer_observations = 0;
    std::size_t waiter_observations = 0;
    std::size_t planner_observations = 0;
    std::size_t reader_observations = 0;
    for (std::size_t index = 0; index < snapshot.count; ++index) {
        REQUIRE(observations[index].valid());
        producer_observations += observations[index].producer ? 1U : 0U;
        waiter_observations +=
            observations[index].reference_token.kind == reusable_reference_kind::waiter ? 1U : 0U;
        planner_observations +=
            observations[index].reference_token.kind == reusable_reference_kind::planner_pin ? 1U
                                                                                             : 0U;
        reader_observations +=
            observations[index].reference_token.kind == reusable_reference_kind::reader_pin ? 1U
                                                                                            : 0U;
    }
    REQUIRE(producer_observations == 1);
    REQUIRE(waiter_observations == 1);
    REQUIRE(planner_observations == 2);
    REQUIRE(reader_observations == 1);

    const auto exact_producer =
        std::find_if(observations.begin(),
                     observations.begin() + snapshot.count,
                     [&exact](const reusable_janitor_owner_observation& observation) noexcept {
                         return observation.producer && observation.reference == exact.reference;
                     });
    REQUIRE(exact_producer != observations.begin() + snapshot.count);
    REQUIRE(exact_producer->entry_kind == cache_entry_kind::exact_final_result);
    REQUIRE(exact_producer->owner == producer(1));
    REQUIRE_FALSE(exact_producer->reference_token.active);

    const auto waiter_observation = std::find_if(
        observations.begin(),
        observations.begin() + snapshot.count,
        [&exact_waiter](const reusable_janitor_owner_observation& observation) noexcept {
            return !observation.producer && observation.reference_token == exact_waiter.token;
        });
    REQUIRE(waiter_observation != observations.begin() + snapshot.count);
    REQUIRE(waiter_observation->entry_kind == cache_entry_kind::exact_final_result);
    REQUIRE(waiter_observation->reference_token.kind == reusable_reference_kind::waiter);

    std::array<reusable_janitor_owner_observation, 2> short_observations{};
    const auto truncated =
        snapshot_reusable_janitor_liveness(fixture.attachment, short_observations, 5);
    REQUIRE(truncated.error == reusable_registry_error::none);
    REQUIRE(truncated.truncated);
    REQUIRE(truncated.count == short_observations.size());
    REQUIRE(truncated.scanned_generations >= 1);

    const reusable_janitor_owner_observation stale_observation = *waiter_observation;
    reusable_reference_token release_copy = exact_waiter.token;
    REQUIRE(release_reusable_reference(fixture.attachment, release_copy) ==
            reusable_registry_error::none);
    auto replacement_waiter = pin_reusable_generation(fixture.attachment,
                                                      exact.reference,
                                                      reusable_reference_kind::waiter,
                                                      exact_waiter.token.owner);
    REQUIRE(replacement_waiter.has_value());
    REQUIRE(replacement_waiter->owner_slot == stale_observation.reference_token.owner_slot);
    REQUIRE(replacement_waiter->owner_sequence != stale_observation.reference_token.owner_sequence);
    REQUIRE(*replacement_waiter != stale_observation.reference_token);
    REQUIRE(release_reusable_reference(fixture.attachment, exact_waiter.token) ==
            reusable_registry_error::stale_reference);

    sampled_owner_liveness stale_dead_sample{
        .observation = stale_observation,
        .alive = false,
    };
    const auto stale_sweep = janitor_reusable_registry(fixture.attachment,
                                                       1'000,
                                                       k_maximum_reusable_janitor_batch,
                                                       reusable_janitor_callbacks{
                                                           .context = &stale_dead_sample,
                                                           .owner_alive = sampled_owner_alive,
                                                       });
    REQUIRE(stale_sweep.error == reusable_registry_error::none);
    REQUIRE(stale_sweep.reaped_waiters == 0);
    REQUIRE(stale_sweep.reaped_planner_pins == 0);
    REQUIRE(stale_sweep.reaped_reader_pins == 0);

    auto invalid_observation = stale_observation;
    invalid_observation.entry_kind = static_cast<cache_entry_kind>(99);
    REQUIRE_FALSE(invalid_observation.valid());
    REQUIRE(release_reusable_reference(fixture.attachment, *replacement_waiter) ==
            reusable_registry_error::none);
    REQUIRE(release_reusable_reference(fixture.attachment, *exact_planner) ==
            reusable_registry_error::none);
    REQUIRE(release_reusable_reference(fixture.attachment, *reusable_planner) ==
            reusable_registry_error::none);
    REQUIRE(release_reusable_reference(fixture.attachment, reusable_reader.token) ==
            reusable_registry_error::none);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("janitor reaps exact dead waiter planner and reader owners",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{4, 16U << 10, 1};
    const auto key = coverage_key(46);
    auto claimed = claim(fixture, key, digest(46), "domain", "generation", 1);
    REQUIRE(claimed);
    auto converting = claim(fixture, key, digest(46), "domain", "generation", 2);
    auto dead_waiter = claim(fixture, key, digest(46), "domain", "generation", 3);
    REQUIRE(converting.token.kind == reusable_reference_kind::waiter);
    REQUIRE(dead_waiter.token.kind == reusable_reference_kind::waiter);

    auto dead_planner = pin_reusable_generation(fixture.attachment,
                                                claimed.reference,
                                                reusable_reference_kind::planner_pin,
                                                reference_owner(400));
    REQUIRE(dead_planner.has_value());
    REQUIRE(static_cast<bool>(publish(fixture, claimed.reference)));

    const std::uint32_t converting_slot = converting.token.owner_slot;
    const std::uint64_t converting_sequence = converting.token.owner_sequence;
    REQUIRE(convert_reusable_waiter_to_reader(fixture.attachment, converting.token) ==
            reusable_registry_error::none);
    REQUIRE(converting.token.kind == reusable_reference_kind::reader_pin);
    REQUIRE(converting.token.owner_slot == converting_slot);
    REQUIRE(converting.token.owner_sequence == converting_sequence);
    REQUIRE(release_reusable_reference(fixture.attachment, converting.token) ==
            reusable_registry_error::none);

    auto dead_reader = lookup_reusable_generation(fixture.attachment,
                                                  key,
                                                  byte_view("domain"),
                                                  digest(46),
                                                  byte_view("generation"),
                                                  reusable_lookup_mode::pin_complete,
                                                  reference_owner(401));
    REQUIRE(dead_reader.error == reusable_registry_error::none);
    auto before = snapshot_reusable_generation(fixture.attachment, claimed.reference);
    REQUIRE(before.has_value());
    REQUIRE(before->waiter_count == 1);
    REQUIRE(before->planner_pin_count == 1);
    REQUIRE(before->reader_pin_count == 1);

    const auto sweep =
        janitor_reusable_registry(fixture.attachment,
                                  1'000,
                                  k_maximum_reusable_janitor_batch,
                                  reusable_janitor_callbacks{
                                      .owner_alive = janitor_only_producers_are_alive,
                                  });
    REQUIRE(sweep.error == reusable_registry_error::none);
    REQUIRE(sweep.reaped_waiters == 1);
    REQUIRE(sweep.reaped_planner_pins == 1);
    REQUIRE(sweep.reaped_reader_pins == 1);
    auto after = snapshot_reusable_generation(fixture.attachment, claimed.reference);
    REQUIRE(after.has_value());
    REQUIRE(after->waiter_count == 0);
    REQUIRE(after->planner_pin_count == 0);
    REQUIRE(after->reader_pin_count == 0);

    REQUIRE(release_reusable_reference(fixture.attachment, dead_waiter.token) ==
            reusable_registry_error::stale_reference);
    REQUIRE(release_reusable_reference(fixture.attachment, *dead_planner) ==
            reusable_registry_error::stale_reference);
    REQUIRE(release_reusable_reference(fixture.attachment, dead_reader.token) ==
            reusable_registry_error::stale_reference);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);

    REQUIRE(invalidate_reusable_generation(fixture.attachment, claimed.reference) ==
            reusable_registry_error::none);
    const auto retirement = janitor_reusable_registry(fixture.attachment, 1'001);
    REQUIRE(retirement.error == reusable_registry_error::none);
    REQUIRE(retirement.retired_invalidated == 1);
}

TEST_CASE("reusable producer diagnostics are bounded ABA-owned and separate",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{4};
    const auto key = coverage_key(46);
    auto claimed =
        claim(fixture, key, digest(68), "diagnostic-domain", "diagnostic-generation", 8, 101);
    REQUIRE(claimed);

    const std::string_view sql = "SELECT payload FROM provider_rows WHERE private_value = 17";
    const std::string_view canonical = "{CUSTOMSCAN :methods BioseqReusableCoverageScan}";
    const std::string_view planned = "Custom Scan (GenericProvider)\n  Provider Workers: 4";
    reusable_diagnostic_text_update update{
        .reference = claimed.reference,
        .producer = producer(8),
        .display_sql = char_view(sql),
        .canonical_plan = char_view(canonical),
        .producer_plan = char_view(planned),
        .canonical_plan_status = reusable_plan_snapshot_status::planned,
        .plan_status = reusable_plan_snapshot_status::planned,
        .canonical_plan_truncated = true,
        .producer_plan_truncated = true,
    };
    REQUIRE(store_reusable_diagnostic_text(fixture.attachment, update) ==
            reusable_registry_error::invalid_state);

    auto wrong_owner = update;
    wrong_owner.producer = producer(9);
    REQUIRE(begin_reusable_build_with_diagnostics(fixture.attachment, wrong_owner) ==
            reusable_registry_error::invalid_state);
    const auto still_claimed = snapshot_reusable_generation(fixture.attachment, claimed.reference);
    REQUIRE(still_claimed.has_value());
    REQUIRE(still_claimed->state == cache_entry_state::claimed);

    auto premature_analyzed = update;
    premature_analyzed.plan_status = reusable_plan_snapshot_status::analyzed;
    REQUIRE(begin_reusable_build_with_diagnostics(fixture.attachment, premature_analyzed) ==
            reusable_registry_error::invalid_argument);
    diagnostics_partition_trace begin_trace{};
    REQUIRE(begin_reusable_build_with_diagnostics(fixture.attachment,
                                                  update,
                                                  reusable_registry_partition_callbacks{
                                                      .context = &begin_trace,
                                                      .acquire_exact = trace_exact_acquire,
                                                      .release_exact = trace_exact_release,
                                                      .acquire_coverage = trace_coverage_acquire,
                                                      .release_coverage = trace_coverage_release,
                                                  }) == reusable_registry_error::none);
    REQUIRE(begin_trace.exact_acquires == 2);
    REQUIRE(begin_trace.exact_releases == 2);
    REQUIRE(begin_trace.coverage_acquires == 0);
    REQUIRE(begin_trace.coverage_releases == 0);
    REQUIRE(begin_reusable_build_with_diagnostics(fixture.attachment, update) ==
            reusable_registry_error::invalid_state);

    std::array<reusable_generation_diagnostics_row, 1> redacted_rows{};
    const auto redacted =
        snapshot_reusable_generation_diagnostics(fixture.attachment, redacted_rows);
    REQUIRE(redacted.error == reusable_registry_error::none);
    REQUIRE(redacted.count == 1);
    REQUIRE(redacted_rows[0].plan_snapshot_status == reusable_plan_snapshot_status::planned);
    REQUIRE(redacted_rows[0].canonical_plan_snapshot_status ==
            reusable_plan_snapshot_status::planned);
    REQUIRE_FALSE(redacted_rows[0].display_sql_truncated);
    REQUIRE(redacted_rows[0].canonical_plan_truncated);
    REQUIRE(redacted_rows[0].producer_plan_truncated);

    std::vector<char> sql_copy(k_reusable_display_sql_capacity);
    std::vector<char> canonical_copy(k_reusable_canonical_plan_capacity);
    std::vector<char> plan_copy(k_reusable_producer_plan_capacity);
    const auto copied = copy_reusable_diagnostic_text(
        fixture.attachment, claimed.reference, sql_copy, canonical_copy, plan_copy);
    REQUIRE(copied.has_value());
    REQUIRE(copied->display_sql_bytes == sql.size());
    REQUIRE(copied->canonical_plan_bytes == canonical.size());
    REQUIRE(copied->producer_plan_bytes == planned.size());
    REQUIRE(copied->canonical_plan_status == reusable_plan_snapshot_status::planned);
    REQUIRE(copied->plan_status == reusable_plan_snapshot_status::planned);
    REQUIRE(copied->canonical_plan_truncated);
    REQUIRE(copied->producer_plan_truncated);
    REQUIRE(std::string_view(sql_copy.data(), copied->display_sql_bytes) == sql);
    REQUIRE(std::string_view(canonical_copy.data(), copied->canonical_plan_bytes) == canonical);
    REQUIRE(std::string_view(plan_copy.data(), copied->producer_plan_bytes) == planned);

    std::array<char, 1> undersized{};
    REQUIRE(copy_reusable_diagnostic_text(
                fixture.attachment, claimed.reference, undersized, canonical_copy, plan_copy)
                .error() == reusable_registry_error::invalid_argument);
    std::string oversized(k_reusable_producer_plan_capacity + 1U, 'x');
    auto oversized_update = update;
    oversized_update.producer_plan = char_view(oversized);
    REQUIRE(store_reusable_diagnostic_text(fixture.attachment, oversized_update) ==
            reusable_registry_error::invalid_argument);

    const reusable_manifest_handle manifest{
        .slot = claimed.reference.generation_slot,
        .slot_sequence = claimed.reference.slot_sequence + 100,
        .id = claimed.reference.id,
    };
    REQUIRE(publish_reusable_generation(fixture.attachment,
                                        reusable_publish_request{
                                            .reference = claimed.reference,
                                            .manifest = manifest,
                                            .completed_monotonic_ns = 202,
                                            .row_count = 4,
                                            .logical_bytes = 700,
                                            .stored_bytes = 300,
                                        }) == reusable_registry_error::none);
    const std::string_view analyzed = "Custom Scan (GenericProvider) (actual rows=4)";
    auto analyzed_update = update;
    analyzed_update.producer_plan = char_view(analyzed);
    analyzed_update.plan_status = reusable_plan_snapshot_status::analyzed;
    analyzed_update.producer_plan_truncated = false;
    REQUIRE(store_reusable_diagnostic_text(fixture.attachment, analyzed_update) ==
            reusable_registry_error::none);
    REQUIRE(store_reusable_diagnostic_text(fixture.attachment, update) ==
            reusable_registry_error::invalid_state);

    auto stale_reference = claimed.reference;
    ++stale_reference.slot_sequence;
    REQUIRE(copy_reusable_diagnostic_text(
                fixture.attachment, stale_reference, sql_copy, canonical_copy, plan_copy)
                .error() == reusable_registry_error::stale_reference);
}

TEST_CASE("reusable build start publishes unavailable plan statuses",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{2};
    auto claimed =
        claim(fixture, coverage_key(48), digest(71), "current-domain", "current-generation", 7);
    REQUIRE(claimed);
    REQUIRE(begin_reusable_build(fixture.attachment, claimed.reference) ==
            reusable_registry_error::none);

    std::vector<char> sql_copy(k_reusable_display_sql_capacity);
    std::vector<char> canonical_copy(k_reusable_canonical_plan_capacity);
    std::vector<char> plan_copy(k_reusable_producer_plan_capacity);
    const auto copied = copy_reusable_diagnostic_text(
        fixture.attachment, claimed.reference, sql_copy, canonical_copy, plan_copy);
    REQUIRE(copied.has_value());
    REQUIRE(copied->plan_status == reusable_plan_snapshot_status::unavailable);
    REQUIRE(copied->canonical_plan_status == reusable_plan_snapshot_status::unavailable);
    REQUIRE(copied->canonical_plan_bytes == 0);
    REQUIRE(std::string_view(sql_copy.data(), copied->display_sql_bytes) ==
            "SQL capture unavailable");
    REQUIRE(copied->producer_plan_bytes == 0);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("reusable analyzed diagnostics may refresh a live producer",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{2};
    auto claimed =
        claim(fixture, coverage_key(50), digest(74), "analyzed-domain", "analyzed-generation", 10);
    REQUIRE(claimed);

    const std::string_view sql = "SELECT payload FROM provider_rows";
    const std::string_view canonical = "{CUSTOMSCAN :provider 10}";
    const std::string_view planned = "Custom Scan (GenericProvider)";
    reusable_diagnostic_text_update update{
        .reference = claimed.reference,
        .producer = producer(10),
        .display_sql = char_view(sql),
        .canonical_plan = char_view(canonical),
        .producer_plan = char_view(planned),
        .canonical_plan_status = reusable_plan_snapshot_status::planned,
        .plan_status = reusable_plan_snapshot_status::planned,
    };
    REQUIRE(begin_reusable_build_with_diagnostics(fixture.attachment, update) ==
            reusable_registry_error::none);

    const std::string_view analyzed = "Custom Scan (GenericProvider) (actual rows=3 loops=1)";
    auto analyzed_update = update;
    analyzed_update.producer_plan = char_view(analyzed);
    analyzed_update.plan_status = reusable_plan_snapshot_status::analyzed;
    REQUIRE(store_reusable_diagnostic_text(fixture.attachment, analyzed_update) ==
            reusable_registry_error::none);
    std::array<reusable_generation_diagnostics_row, 1> rows{};
    const auto snapshot = snapshot_reusable_generation_diagnostics(fixture.attachment, rows);
    REQUIRE(snapshot.error == reusable_registry_error::none);
    REQUIRE(snapshot.count == 1);
    const auto* building = diagnostics_row(rows, claimed.reference);
    REQUIRE(building != nullptr);
    REQUIRE(building->state == cache_entry_state::building);
    REQUIRE(building->plan_snapshot_status == reusable_plan_snapshot_status::analyzed);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
    REQUIRE(store_reusable_diagnostic_text(fixture.attachment, update) ==
            reusable_registry_error::invalid_state);

    REQUIRE(fail_reusable_generation(
                fixture.attachment, claimed.reference, cache_failure_code::producer_error) ==
            reusable_registry_error::none);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("reusable SQL and plan captures fail independently",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{4};
    const std::string_view sql = "SELECT payload FROM provider_rows";
    const std::string_view canonical = "{CUSTOMSCAN :provider 7}";
    const std::string_view human = "Custom Scan (GenericProvider)";

    auto human_unavailable =
        claim(fixture, coverage_key(49), digest(72), "capture-domain", "human-unavailable", 8);
    REQUIRE(human_unavailable);
    REQUIRE(begin_reusable_build_with_diagnostics(
                fixture.attachment,
                reusable_diagnostic_text_update{
                    .reference = human_unavailable.reference,
                    .producer = producer(8),
                    .display_sql = char_view(sql),
                    .canonical_plan = char_view(canonical),
                    .canonical_plan_status = reusable_plan_snapshot_status::planned,
                }) == reusable_registry_error::none);

    auto canonical_unavailable =
        claim(fixture, coverage_key(49), digest(73), "capture-domain", "canonical-unavailable", 9);
    REQUIRE(canonical_unavailable);
    REQUIRE(begin_reusable_build_with_diagnostics(
                fixture.attachment,
                reusable_diagnostic_text_update{
                    .reference = canonical_unavailable.reference,
                    .producer = producer(9),
                    .display_sql = char_view(sql),
                    .producer_plan = char_view(human),
                    .plan_status = reusable_plan_snapshot_status::planned,
                }) == reusable_registry_error::none);

    std::vector<char> sql_copy(k_reusable_display_sql_capacity);
    std::vector<char> canonical_copy(k_reusable_canonical_plan_capacity);
    std::vector<char> plan_copy(k_reusable_producer_plan_capacity);
    const auto first = copy_reusable_diagnostic_text(
        fixture.attachment, human_unavailable.reference, sql_copy, canonical_copy, plan_copy);
    REQUIRE(first.has_value());
    REQUIRE(first->display_sql_bytes == sql.size());
    REQUIRE(first->canonical_plan_bytes == canonical.size());
    REQUIRE(first->producer_plan_bytes == 0);
    REQUIRE(first->plan_status == reusable_plan_snapshot_status::unavailable);

    std::fill(sql_copy.begin(), sql_copy.end(), 0);
    std::fill(canonical_copy.begin(), canonical_copy.end(), 0);
    std::fill(plan_copy.begin(), plan_copy.end(), 0);
    const auto second = copy_reusable_diagnostic_text(
        fixture.attachment, canonical_unavailable.reference, sql_copy, canonical_copy, plan_copy);
    REQUIRE(second.has_value());
    REQUIRE(second->display_sql_bytes == sql.size());
    REQUIRE(second->canonical_plan_bytes == 0);
    REQUIRE(second->producer_plan_bytes == human.size());
    REQUIRE(second->canonical_plan_status == reusable_plan_snapshot_status::unavailable);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("diagnostic arena accepts each independent boundary exactly",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{2, 20U << 20, 1};
    auto claimed =
        claim(fixture, coverage_key(54), digest(91), "boundary-domain", "boundary-generation", 1);
    REQUIRE(claimed);
    const std::uint64_t baseline_free =
        reusable_registry_snapshot(fixture.attachment).arena_free_bytes;

    const std::string display_below(k_reusable_display_sql_capacity - 1U, 's');
    const std::string canonical_below(k_reusable_canonical_plan_capacity - 1U, 'c');
    const std::string producer_below(k_reusable_producer_plan_capacity - 1U, 'p');
    reusable_diagnostic_text_update update{
        .reference = claimed.reference,
        .producer = producer(1),
        .display_sql = char_view(display_below),
        .canonical_plan = char_view(canonical_below),
        .producer_plan = char_view(producer_below),
        .canonical_plan_status = reusable_plan_snapshot_status::planned,
        .plan_status = reusable_plan_snapshot_status::planned,
    };
    REQUIRE(begin_reusable_build_with_diagnostics(fixture.attachment, update) ==
            reusable_registry_error::none);
    REQUIRE(reusable_registry_snapshot(fixture.attachment).arena_free_bytes ==
            baseline_free - aligned_eight(display_below.size()) -
                aligned_eight(canonical_below.size()) - aligned_eight(producer_below.size()));

    const std::string display_at(k_reusable_display_sql_capacity, 'S');
    const std::string canonical_at(k_reusable_canonical_plan_capacity, 'C');
    const std::string producer_at(k_reusable_producer_plan_capacity, 'P');
    update.display_sql = char_view(display_at);
    update.canonical_plan = char_view(canonical_at);
    update.producer_plan = char_view(producer_at);
    update.display_sql_truncated = true;
    update.canonical_plan_truncated = true;
    update.producer_plan_truncated = true;
    REQUIRE(store_reusable_diagnostic_text(fixture.attachment, update) ==
            reusable_registry_error::none);
    const std::uint64_t exact_boundary_bytes = aligned_eight(display_at.size()) +
                                               aligned_eight(canonical_at.size()) +
                                               aligned_eight(producer_at.size());
    REQUIRE(reusable_registry_snapshot(fixture.attachment).arena_free_bytes ==
            baseline_free - exact_boundary_bytes);

    std::vector<char> display_copy(display_at.size());
    std::vector<char> canonical_copy(canonical_at.size());
    std::vector<char> producer_copy(producer_at.size());
    const auto copied = copy_reusable_diagnostic_text(
        fixture.attachment, claimed.reference, display_copy, canonical_copy, producer_copy);
    REQUIRE(copied.has_value());
    REQUIRE(copied->display_sql_bytes == display_at.size());
    REQUIRE(copied->canonical_plan_bytes == canonical_at.size());
    REQUIRE(copied->producer_plan_bytes == producer_at.size());
    REQUIRE(copied->display_sql_truncated);
    REQUIRE(copied->canonical_plan_truncated);
    REQUIRE(copied->producer_plan_truncated);
    REQUIRE(std::equal(display_at.begin(), display_at.end(), display_copy.begin()));
    REQUIRE(std::equal(canonical_at.begin(), canonical_at.end(), canonical_copy.begin()));
    REQUIRE(std::equal(producer_at.begin(), producer_at.end(), producer_copy.begin()));

    const std::uint64_t before_rejections =
        reusable_registry_snapshot(fixture.attachment).arena_free_bytes;
    {
        const std::string too_large(k_reusable_display_sql_capacity + 1U, 'x');
        auto rejected = update;
        rejected.display_sql = char_view(too_large);
        REQUIRE(store_reusable_diagnostic_text(fixture.attachment, rejected) ==
                reusable_registry_error::invalid_argument);
    }
    {
        const std::string too_large(k_reusable_canonical_plan_capacity + 1U, 'x');
        auto rejected = update;
        rejected.canonical_plan = char_view(too_large);
        REQUIRE(store_reusable_diagnostic_text(fixture.attachment, rejected) ==
                reusable_registry_error::invalid_argument);
    }
    {
        const std::string too_large(k_reusable_producer_plan_capacity + 1U, 'x');
        auto rejected = update;
        rejected.producer_plan = char_view(too_large);
        REQUIRE(store_reusable_diagnostic_text(fixture.attachment, rejected) ==
                reusable_registry_error::invalid_argument);
    }
    REQUIRE(reusable_registry_snapshot(fixture.attachment).arena_free_bytes == before_rejections);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("diagnostic replacement and retired reclaim release every range",
          "[component:cache][layer:unit][feature:registry]") {
    constexpr std::uint32_t arena_bytes = 64U << 10;
    registry_fixture fixture{1, arena_bytes, 1};
    auto claimed =
        claim(fixture, coverage_key(55), digest(92), "replace-domain", "replace-generation", 2);
    REQUIRE(claimed);
    const std::uint64_t baseline_free =
        reusable_registry_snapshot(fixture.attachment).arena_free_bytes;

    const std::string old_sql(9, 's');
    const std::string old_canonical(17, 'c');
    const std::string old_producer(25, 'p');
    reusable_diagnostic_text_update update{
        .reference = claimed.reference,
        .producer = producer(2),
        .display_sql = char_view(old_sql),
        .canonical_plan = char_view(old_canonical),
        .producer_plan = char_view(old_producer),
        .canonical_plan_status = reusable_plan_snapshot_status::planned,
        .plan_status = reusable_plan_snapshot_status::planned,
    };
    REQUIRE(begin_reusable_build_with_diagnostics(fixture.attachment, update) ==
            reusable_registry_error::none);
    reusable_shared_generation& record = shared_generation(fixture, claimed.reference);
    REQUIRE(record.display_sql.offset % 8U == 0);
    REQUIRE(record.canonical_plan.offset % 8U == 0);
    REQUIRE(record.producer_plan.offset % 8U == 0);
    REQUIRE(record.display_sql.offset != record.canonical_plan.offset);
    REQUIRE(record.display_sql.offset != record.producer_plan.offset);
    REQUIRE(record.canonical_plan.offset != record.producer_plan.offset);

    const std::string new_sql(33, 'S');
    const std::string new_canonical(41, 'C');
    const std::string new_producer(49, 'P');
    update.display_sql = char_view(new_sql);
    update.canonical_plan = char_view(new_canonical);
    update.producer_plan = char_view(new_producer);
    REQUIRE(store_reusable_diagnostic_text(fixture.attachment, update) ==
            reusable_registry_error::none);
    const std::uint64_t replacement_bytes = aligned_eight(new_sql.size()) +
                                            aligned_eight(new_canonical.size()) +
                                            aligned_eight(new_producer.size());
    REQUIRE(reusable_registry_snapshot(fixture.attachment).arena_free_bytes ==
            baseline_free - replacement_bytes);

    const reusable_manifest_handle manifest{
        .slot = claimed.reference.generation_slot,
        .slot_sequence = claimed.reference.slot_sequence + 100,
        .id = claimed.reference.id,
    };
    REQUIRE(publish_reusable_generation(fixture.attachment,
                                        reusable_publish_request{
                                            .reference = claimed.reference,
                                            .manifest = manifest,
                                            .completed_monotonic_ns = 300,
                                        }) == reusable_registry_error::none);
    const auto retired = retire_reusable_generation(fixture.attachment, claimed.reference);
    REQUIRE(retired.has_value());
    REQUIRE(*retired == manifest);
    REQUIRE(reusable_registry_snapshot(fixture.attachment).arena_free_bytes ==
            baseline_free - replacement_bytes);
    REQUIRE(release_and_finalize_one_retired(fixture) == reusable_registry_error::none);
    REQUIRE(reusable_registry_snapshot(fixture.attachment).arena_free_bytes == arena_bytes);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("diagnostic publication revalidates producer and ABA identity",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{1, 1U << 20, 1};
    const auto key = coverage_key(56);
    auto claimed = claim(fixture, key, digest(93), "aba-domain", "aba-generation", 3);
    REQUIRE(claimed);
    const std::string_view sql = "SELECT payload FROM provider_rows";
    const std::string_view plan = "Custom Scan (GenericProvider)";
    reusable_diagnostic_text_update update{
        .reference = claimed.reference,
        .producer = producer(3),
        .display_sql = char_view(sql),
        .producer_plan = char_view(plan),
        .plan_status = reusable_plan_snapshot_status::planned,
    };
    const std::uint64_t initial_free =
        reusable_registry_snapshot(fixture.attachment).arena_free_bytes;
    auto wrong_producer = update;
    wrong_producer.producer = producer(4);
    REQUIRE(begin_reusable_build_with_diagnostics(fixture.attachment, wrong_producer) ==
            reusable_registry_error::invalid_state);
    REQUIRE(reusable_registry_snapshot(fixture.attachment).arena_free_bytes == initial_free);

    reusable_shared_generation& record = shared_generation(fixture, claimed.reference);
    const std::uint64_t original_slot_sequence = record.slot_sequence;
    diagnostic_aba_trace trace{
        .generation = &record,
        .mutate_on_first_release = true,
    };
    const reusable_registry_partition_callbacks callbacks{
        .context = &trace,
        .acquire_exact = diagnostic_aba_acquire,
        .release_exact = diagnostic_aba_release,
        .acquire_coverage = diagnostic_aba_coverage_noop,
        .release_coverage = diagnostic_aba_coverage_noop,
    };
    REQUIRE(begin_reusable_build_with_diagnostics(fixture.attachment, update, callbacks) ==
            reusable_registry_error::stale_reference);
    REQUIRE(trace.exact_acquires == 2);
    REQUIRE(trace.exact_releases == 2);
    REQUIRE(reusable_registry_snapshot(fixture.attachment).arena_free_bytes == initial_free);
    REQUIRE(record.state == cache_entry_state::claimed);
    REQUIRE(record.display_sql.length == 0);
    record.slot_sequence = original_slot_sequence;
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);

    REQUIRE(begin_reusable_build_with_diagnostics(fixture.attachment, update) ==
            reusable_registry_error::none);
    const reusable_manifest_handle manifest{
        .slot = claimed.reference.generation_slot,
        .slot_sequence = claimed.reference.slot_sequence + 100,
        .id = claimed.reference.id,
    };
    REQUIRE(publish_reusable_generation(fixture.attachment,
                                        reusable_publish_request{
                                            .reference = claimed.reference,
                                            .manifest = manifest,
                                            .completed_monotonic_ns = 400,
                                        }) == reusable_registry_error::none);
    REQUIRE(retire_reusable_generation(fixture.attachment, claimed.reference).has_value());
    REQUIRE(release_and_finalize_one_retired(fixture) == reusable_registry_error::none);
    auto replacement = claim(fixture, key, digest(93), "aba-domain", "aba-generation", 5);
    REQUIRE(replacement);
    REQUIRE(replacement.reference.generation_slot == claimed.reference.generation_slot);
    REQUIRE(replacement.reference.slot_sequence != claimed.reference.slot_sequence);
    const std::uint64_t before_stale =
        reusable_registry_snapshot(fixture.attachment).arena_free_bytes;
    REQUIRE(store_reusable_diagnostic_text(fixture.attachment, update) ==
            reusable_registry_error::stale_reference);
    REQUIRE(reusable_registry_snapshot(fixture.attachment).arena_free_bytes == before_stale);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("diagnostic arena OOM leaves prior text and state unchanged",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{1, 352, 1};
    auto claimed = claim(fixture, coverage_key(57), digest(94), "d", "g", 6);
    REQUIRE(claimed);
    const std::string old_sql(16, 's');
    const std::string old_canonical(16, 'c');
    const std::string old_producer(16, 'p');
    reusable_diagnostic_text_update update{
        .reference = claimed.reference,
        .producer = producer(6),
        .display_sql = char_view(old_sql),
        .canonical_plan = char_view(old_canonical),
        .producer_plan = char_view(old_producer),
        .canonical_plan_status = reusable_plan_snapshot_status::planned,
        .plan_status = reusable_plan_snapshot_status::planned,
    };
    REQUIRE(begin_reusable_build_with_diagnostics(fixture.attachment, update) ==
            reusable_registry_error::none);
    const reusable_shared_generation& before_record = shared_generation(fixture, claimed.reference);
    const reusable_shared_byte_range old_sql_range = before_record.display_sql;
    const reusable_shared_byte_range old_canonical_range = before_record.canonical_plan;
    const reusable_shared_byte_range old_producer_range = before_record.producer_plan;
    const std::uint64_t before_free =
        reusable_registry_snapshot(fixture.attachment).arena_free_bytes;

    const std::string too_large_sql(80, 'S');
    const std::string too_large_canonical(80, 'C');
    const std::string too_large_producer(80, 'P');
    update.display_sql = char_view(too_large_sql);
    update.canonical_plan = char_view(too_large_canonical);
    update.producer_plan = char_view(too_large_producer);
    REQUIRE(store_reusable_diagnostic_text(fixture.attachment, update) ==
            reusable_registry_error::byte_arena_full);
    const reusable_shared_generation& after_record = shared_generation(fixture, claimed.reference);
    REQUIRE(after_record.state == cache_entry_state::building);
    REQUIRE(after_record.display_sql.offset == old_sql_range.offset);
    REQUIRE(after_record.display_sql.length == old_sql_range.length);
    REQUIRE(after_record.canonical_plan.offset == old_canonical_range.offset);
    REQUIRE(after_record.canonical_plan.length == old_canonical_range.length);
    REQUIRE(after_record.producer_plan.offset == old_producer_range.offset);
    REQUIRE(after_record.producer_plan.length == old_producer_range.length);
    REQUIRE(reusable_registry_snapshot(fixture.attachment).arena_free_bytes == before_free);
    std::array<char, 16> sql_copy{};
    std::array<char, 16> canonical_copy{};
    std::array<char, 16> producer_copy{};
    const auto copied = copy_reusable_diagnostic_text(
        fixture.attachment, claimed.reference, sql_copy, canonical_copy, producer_copy);
    REQUIRE(copied.has_value());
    REQUIRE(std::string_view(sql_copy.data(), sql_copy.size()) == old_sql);
    REQUIRE(std::string_view(canonical_copy.data(), canonical_copy.size()) == old_canonical);
    REQUIRE(std::string_view(producer_copy.data(), producer_copy.size()) == old_producer);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);

    registry_fixture begin_fixture{1, 280, 1};
    auto begin_claimed = claim(begin_fixture, coverage_key(58), digest(95), "d", "g", 7);
    REQUIRE(begin_claimed);
    const std::string begin_sql(40, 's');
    const std::string begin_plan(40, 'p');
    const std::uint64_t begin_free =
        reusable_registry_snapshot(begin_fixture.attachment).arena_free_bytes;
    REQUIRE(begin_reusable_build_with_diagnostics(
                begin_fixture.attachment,
                reusable_diagnostic_text_update{
                    .reference = begin_claimed.reference,
                    .producer = producer(7),
                    .display_sql = char_view(begin_sql),
                    .producer_plan = char_view(begin_plan),
                    .plan_status = reusable_plan_snapshot_status::planned,
                }) == reusable_registry_error::byte_arena_full);
    REQUIRE(
        snapshot_reusable_generation(begin_fixture.attachment, begin_claimed.reference)->state ==
        cache_entry_state::claimed);
    REQUIRE(reusable_registry_snapshot(begin_fixture.attachment).arena_free_bytes == begin_free);
    REQUIRE(validate_reusable_registry(begin_fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("deep validation rejects diagnostic arena range corruption",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{1, 4U << 10, 1};
    auto claimed = claim(
        fixture, coverage_key(59), digest(96), "corruption-domain", "corruption-generation", 8);
    REQUIRE(claimed);
    const std::string_view sql = "SELECT payload";
    const std::string_view canonical = "{CUSTOMSCAN}";
    const std::string_view plan = "Custom Scan";
    REQUIRE(begin_reusable_build_with_diagnostics(
                fixture.attachment,
                reusable_diagnostic_text_update{
                    .reference = claimed.reference,
                    .producer = producer(8),
                    .display_sql = char_view(sql),
                    .canonical_plan = char_view(canonical),
                    .producer_plan = char_view(plan),
                    .canonical_plan_status = reusable_plan_snapshot_status::planned,
                    .plan_status = reusable_plan_snapshot_status::planned,
                }) == reusable_registry_error::none);
    reusable_shared_generation& record = shared_generation(fixture, claimed.reference);
    const reusable_shared_byte_range original_display = record.display_sql;
    const reusable_shared_byte_range original_canonical = record.canonical_plan;
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);

    record.display_sql.length = k_reusable_display_sql_capacity + 1U;
    REQUIRE(validate_reusable_registry(fixture.attachment) ==
            reusable_registry_error::corrupt_generation);
    record.display_sql = original_display;

    ++record.display_sql.offset;
    REQUIRE(validate_reusable_registry(fixture.attachment) ==
            reusable_registry_error::corrupt_generation);
    record.display_sql = original_display;

    record.display_sql.offset =
        fixture.request_.layout.byte_arena_offset + fixture.request_.config.byte_arena_bytes;
    REQUIRE(validate_reusable_registry(fixture.attachment) ==
            reusable_registry_error::corrupt_generation);
    record.display_sql = original_display;

    record.display_sql.offset = record.allocation_offset;
    REQUIRE(validate_reusable_registry(fixture.attachment) ==
            reusable_registry_error::corrupt_generation);
    record.display_sql = original_display;

    record.canonical_plan.offset = record.display_sql.offset;
    REQUIRE(validate_reusable_registry(fixture.attachment) ==
            reusable_registry_error::corrupt_generation);
    record.canonical_plan = original_canonical;

    char& first_display_byte =
        *reinterpret_cast<char*>(fixture.attachment.region.data() + record.display_sql.offset);
    const char original_byte = first_display_byte;
    first_display_byte = 0;
    REQUIRE(validate_reusable_registry(fixture.attachment) ==
            reusable_registry_error::corrupt_generation);
    first_display_byte = original_byte;
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("reusable diagnostics copy bounded redacted generation rows",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{8, 32U << 10, 1};
    const auto key = coverage_key(47);

    auto claimed = claim(fixture,
                         key,
                         digest(70),
                         "admin-domain",
                         "claimed",
                         1,
                         101,
                         60'001,
                         "provider-private-secret");
    REQUIRE(claimed);
    auto waiter = claim(fixture, key, digest(70), "admin-domain", "claimed", 10);
    REQUIRE(waiter.disposition == reusable_claim_disposition::existing_in_flight);

    auto building = claim(fixture, key, digest(71), "admin-domain", "building", 2, 102);
    REQUIRE(building);
    REQUIRE(begin_reusable_build(fixture.attachment, building.reference) ==
            reusable_registry_error::none);

    auto complete = claim(fixture, key, digest(72), "admin-domain", "complete", 3, 103);
    REQUIRE(complete);
    const reusable_manifest_handle complete_manifest = publish(fixture, complete.reference, 203);
    REQUIRE(static_cast<bool>(complete_manifest));
    auto planner_pin = pin_reusable_generation(fixture.attachment,
                                               complete.reference,
                                               reusable_reference_kind::planner_pin,
                                               reference_owner(700));
    REQUIRE(planner_pin.has_value());
    auto reader = lookup_reusable_generation(fixture.attachment,
                                             key,
                                             byte_view("admin-domain"),
                                             digest(72),
                                             byte_view("complete"),
                                             reusable_lookup_mode::pin_complete,
                                             reference_owner(701));
    REQUIRE(reader.error == reusable_registry_error::none);

    auto failed = claim(fixture, key, digest(73), "admin-domain", "failed", 4, 104);
    REQUIRE(failed);
    REQUIRE(begin_reusable_build(fixture.attachment, failed.reference) ==
            reusable_registry_error::none);
    REQUIRE(fail_reusable_generation(
                fixture.attachment, failed.reference, cache_failure_code::producer_error) ==
            reusable_registry_error::none);

    std::array<reusable_generation_diagnostics_row, 4> rows{};
    diagnostics_partition_trace trace{};
    const auto snapshot =
        snapshot_reusable_generation_diagnostics(fixture.attachment,
                                                 rows,
                                                 reusable_registry_partition_callbacks{
                                                     .context = &trace,
                                                     .acquire_exact = trace_exact_acquire,
                                                     .release_exact = trace_exact_release,
                                                     .acquire_coverage = trace_coverage_acquire,
                                                     .release_coverage = trace_coverage_release,
                                                 });
    REQUIRE(snapshot.error == reusable_registry_error::none);
    REQUIRE(snapshot.indexed_generation_count == rows.size());
    REQUIRE(snapshot.count == rows.size());
    REQUIRE_FALSE(snapshot.truncated);
    REQUIRE(trace.exact_acquires == rows.size());
    REQUIRE(trace.exact_releases == rows.size());
    REQUIRE(trace.coverage_acquires == 0);
    REQUIRE(trace.coverage_releases == 0);

    const auto* claimed_row = diagnostics_row(rows, claimed.reference);
    REQUIRE(claimed_row != nullptr);
    REQUIRE(claimed_row->entry_kind == cache_entry_kind::reusable_coverage);
    REQUIRE(claimed_row->state == cache_entry_state::claimed);
    REQUIRE(claimed_row->failure == cache_failure_code::none);
    REQUIRE(claimed_row->producer_pid == producer(1).pid);
    REQUIRE(claimed_row->waiter_count == 1);
    REQUIRE(claimed_row->provider_id == 7);
    REQUIRE(claimed_row->provider_abi_revision == 2);
    REQUIRE(claimed_row->provider_domain_abi_revision == 3);
    REQUIRE(claimed_row->dependency_count == 2);
    REQUIRE(claimed_row->database_oid == 11);
    REQUIRE(claimed_row->current_role_oid == 47);
    REQUIRE(claimed_row->dependency_fingerprint == digest(104));
    REQUIRE(claimed_row->compatibility_family_id == 41);
    REQUIRE(claimed_row->capability_mask == 0b1111);
    REQUIRE(claimed_row->observability == write_observability::provider_complete);
    REQUIRE(claimed_row->estimated_covered_rows == 50);
    REQUIRE(claimed_row->producer_ttl_ms == 60'001);
    REQUIRE(claimed_row->started_monotonic_ns == 101);

    const auto* building_row = diagnostics_row(rows, building.reference);
    REQUIRE(building_row != nullptr);
    REQUIRE(building_row->state == cache_entry_state::building);
    REQUIRE(building_row->failure == cache_failure_code::none);

    const auto* complete_row = diagnostics_row(rows, complete.reference);
    REQUIRE(complete_row != nullptr);
    REQUIRE(complete_row->state == cache_entry_state::complete);
    REQUIRE(complete_row->manifest == complete_manifest);
    REQUIRE(complete_row->row_count == 17);
    REQUIRE(complete_row->logical_bytes == 700);
    REQUIRE(complete_row->stored_bytes == 300);
    REQUIRE(complete_row->planner_pin_count == 1);
    REQUIRE(complete_row->reader_pin_count == 1);
    REQUIRE(complete_row->completed_monotonic_ns == 203);
    REQUIRE(complete_row->last_access_monotonic_ns == 203);

    const auto* failed_row = diagnostics_row(rows, failed.reference);
    REQUIRE(failed_row != nullptr);
    REQUIRE(failed_row->state == cache_entry_state::failed);
    REQUIRE(failed_row->failure == cache_failure_code::producer_error);

    std::array<reusable_generation_diagnostics_row, 2> bounded_rows{};
    const auto bounded = snapshot_reusable_generation_diagnostics(fixture.attachment, bounded_rows);
    REQUIRE(bounded.error == reusable_registry_error::none);
    REQUIRE(bounded.indexed_generation_count == rows.size());
    REQUIRE(bounded.count == bounded_rows.size());
    REQUIRE(bounded.truncated);

    const auto count_only = snapshot_reusable_generation_diagnostics(
        fixture.attachment, std::span<reusable_generation_diagnostics_row>{});
    REQUIRE(count_only.error == reusable_registry_error::none);
    REQUIRE(count_only.indexed_generation_count == rows.size());
    REQUIRE(count_only.count == 0);
    REQUIRE(count_only.truncated);

    REQUIRE(release_reusable_reference(fixture.attachment, waiter.token) ==
            reusable_registry_error::none);
    REQUIRE(release_reusable_reference(fixture.attachment, *planner_pin) ==
            reusable_registry_error::none);
    REQUIRE(release_reusable_reference(fixture.attachment, reader.token) ==
            reusable_registry_error::none);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("bounded reusable invalidation is collision and ABA safe",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{8, 32U << 10, 1};
    const auto key = coverage_key(60);
    const auto other_security_key = coverage_key(61);
    const auto other_database_key = coverage_key(60, 90, 12);
    const cache_digest_256 forced_digest = digest(70);

    reusable_generation_metadata shard_a_metadata = metadata();
    shard_a_metadata.physical_identity = physical_identity(10);
    reusable_generation_metadata shard_b_metadata = shard_a_metadata;
    shard_b_metadata.physical_identity = physical_identity(11);
    reusable_generation_metadata other_provider_metadata = shard_a_metadata;
    other_provider_metadata.provider_id = 8;

    auto shard_a =
        claim_with_metadata(fixture, key, forced_digest, "domain", "shard-a", 1, shard_a_metadata);
    auto shard_b =
        claim_with_metadata(fixture, key, forced_digest, "domain", "shard-b", 2, shard_b_metadata);
    auto other_security = claim_with_metadata(fixture,
                                              other_security_key,
                                              forced_digest,
                                              "domain",
                                              "other-security",
                                              3,
                                              shard_a_metadata);
    auto other_database = claim_with_metadata(fixture,
                                              other_database_key,
                                              forced_digest,
                                              "domain",
                                              "other-database",
                                              4,
                                              shard_a_metadata);
    auto other_provider = claim_with_metadata(
        fixture, key, forced_digest, "domain", "other-provider", 5, other_provider_metadata);
    auto inflight =
        claim_with_metadata(fixture, key, forced_digest, "domain", "inflight", 6, shard_a_metadata);
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
            reusable_registry_error::none);

    auto pinned_reader = lookup_reusable_generation(fixture.attachment,
                                                    key,
                                                    byte_view("domain"),
                                                    forced_digest,
                                                    byte_view("shard-a"),
                                                    reusable_lookup_mode::pin_complete,
                                                    reference_owner(500));
    REQUIRE(pinned_reader.error == reusable_registry_error::none);

    reusable_invalidation_selector selector{
        .database_oid = key.security_namespace.database_oid,
        .provider_id = shard_a_metadata.provider_id,
        .security_namespace = key.security_namespace,
        .physical_identity = shard_a_metadata.physical_identity,
        .dependency_fingerprint = shard_a_metadata.dependency_fingerprint,
        .schema_fingerprint = shard_a_metadata.schema_fingerprint,
        .mapping_fingerprint = shard_a_metadata.mapping_fingerprint,
        .lineage_fingerprint = shard_a_metadata.lineage_fingerprint,
        .observability = write_observability::provider_complete,
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
    std::vector<reusable_generation_reference> affected;
    std::vector<reusable_manifest_handle> affected_manifests;
    std::vector<cache_entry_state> previous_states;
    std::uint32_t scanned = 0;
    std::uint32_t matched = 0;
    std::uint32_t invalidated_complete = 0;
    std::uint32_t failed_inflight = 0;
    std::uint32_t referenced = 0;
    std::uint32_t reader_pins = 0;
    bool done = false;
    while (!done) {
        const auto batch = invalidate_reusable_generations(fixture.attachment, selector);
        REQUIRE(batch.error == reusable_registry_error::none);
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
    REQUIRE(previous_states[0] == cache_entry_state::complete);
    REQUIRE(previous_states[1] == cache_entry_state::building);

    auto shard_a_after = snapshot_reusable_generation(fixture.attachment, shard_a.reference);
    auto inflight_after = snapshot_reusable_generation(fixture.attachment, inflight.reference);
    REQUIRE(shard_a_after->state == cache_entry_state::invalidated);
    REQUIRE(shard_a_after->failure == cache_failure_code::dependency_changed);
    REQUIRE(inflight_after->state == cache_entry_state::failed);
    REQUIRE(inflight_after->failure == cache_failure_code::dependency_changed);
    REQUIRE(snapshot_reusable_generation(fixture.attachment, shard_b.reference)->state ==
            cache_entry_state::complete);
    REQUIRE(snapshot_reusable_generation(fixture.attachment, other_security.reference)->state ==
            cache_entry_state::complete);
    REQUIRE(snapshot_reusable_generation(fixture.attachment, other_database.reference)->state ==
            cache_entry_state::complete);
    REQUIRE(snapshot_reusable_generation(fixture.attachment, other_provider.reference)->state ==
            cache_entry_state::complete);

    selector.cursor = 0;
    selector.maximum_scan = k_maximum_reusable_invalidation_scan;
    const auto repeated = invalidate_reusable_generations(fixture.attachment, selector);
    REQUIRE(repeated.error == reusable_registry_error::none);
    REQUIRE(repeated.done);
    REQUIRE(repeated.matched_generations == 2);
    REQUIRE(repeated.already_invalidated == 1);
    REQUIRE(repeated.already_failed == 1);
    REQUIRE(repeated.affected_count == 0);
    REQUIRE(repeated.reader_pins == 1);

    auto pinned_retirement = retire_reusable_generation(fixture.attachment, shard_a.reference);
    REQUIRE_FALSE(pinned_retirement.has_value());
    REQUIRE(pinned_retirement.error() == reusable_registry_error::invalid_state);
    REQUIRE(release_reusable_reference(fixture.attachment, pinned_reader.token) ==
            reusable_registry_error::none);
    auto retired = retire_reusable_generation(fixture.attachment, shard_a.reference);
    REQUIRE(retired.has_value());
    REQUIRE(retired->id == shard_a.reference.id);
    REQUIRE(release_and_finalize_one_retired(fixture) == reusable_registry_error::none);
    auto replacement =
        claim_with_metadata(fixture, key, digest(71), "domain", "replacement", 7, shard_a_metadata);
    REQUIRE(replacement);
    REQUIRE(replacement.reference.generation_slot == shard_a.reference.generation_slot);
    REQUIRE(replacement.reference.slot_sequence != shard_a.reference.slot_sequence);
    auto stale = snapshot_reusable_generation(fixture.attachment, affected.front());
    REQUIRE_FALSE(stale.has_value());
    REQUIRE(stale.error() == reusable_registry_error::stale_reference);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("reusable invalidation observability and coarse scope are explicit",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{4, 16U << 10, 1};
    const auto key = coverage_key(70, 91, 70);
    const auto other_database_key = coverage_key(70, 91, 71);
    reusable_generation_metadata precise_metadata = metadata();
    precise_metadata.physical_identity = physical_identity(20);
    reusable_generation_metadata other_provider_metadata = precise_metadata;
    other_provider_metadata.provider_id = 8;

    auto precise =
        claim_with_metadata(fixture, key, digest(80), "domain", "precise", 1, precise_metadata);
    auto other_provider = claim_with_metadata(
        fixture, key, digest(81), "domain", "other-provider", 2, other_provider_metadata);
    auto other_database = claim_with_metadata(
        fixture, other_database_key, digest(82), "domain", "other-database", 3, precise_metadata);
    REQUIRE(static_cast<bool>(publish(fixture, precise.reference)));
    REQUIRE(static_cast<bool>(publish(fixture, other_provider.reference)));
    REQUIRE(static_cast<bool>(publish(fixture, other_database.reference)));

    REQUIRE(invalidate_reusable_generations(fixture.attachment,
                                            reusable_invalidation_selector{.database_oid = 70})
                .error == reusable_registry_error::invalid_argument);

    const auto precise_result =
        invalidate_reusable_generations(fixture.attachment,
                                        reusable_invalidation_selector{
                                            .database_oid = 70,
                                            .provider_id = 7,
                                            .observability = write_observability::provider_complete,
                                            .match_provider_id = true,
                                            .match_observability = true,
                                        });
    REQUIRE(precise_result.error == reusable_registry_error::none);
    REQUIRE(precise_result.invalidated_complete == 1);
    REQUIRE(precise_result.affected_references[0] == precise.reference);
    REQUIRE(snapshot_reusable_generation(fixture.attachment, other_provider.reference)->state ==
            cache_entry_state::complete);

    reusable_invalidation_selector ambiguous{
        .database_oid = 70,
        .provider_id = 7,
        .match_provider_id = true,
        .coarse_database_fallback = true,
    };
    REQUIRE(invalidate_reusable_generations(fixture.attachment, ambiguous).error ==
            reusable_registry_error::invalid_argument);

    const auto coarse_result = invalidate_reusable_generations(fixture.attachment,
                                                               reusable_invalidation_selector{
                                                                   .database_oid = 70,
                                                                   .coarse_database_fallback = true,
                                                               });
    REQUIRE(coarse_result.error == reusable_registry_error::none);
    REQUIRE(coarse_result.matched_generations == 2);
    REQUIRE(coarse_result.invalidated_complete == 1);
    REQUIRE(coarse_result.already_invalidated == 1);
    REQUIRE(snapshot_reusable_generation(fixture.attachment, other_database.reference)->state ==
            cache_entry_state::complete);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("janitor fails dead owners and retires expired manifests",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{4, 16U << 10, 1};
    const auto key = coverage_key(50);
    auto dead = claim(fixture, key, digest(50), "domain", "dead", 1);
    REQUIRE(dead);
    REQUIRE(begin_reusable_build(fixture.attachment, dead.reference) ==
            reusable_registry_error::none);
    auto live_waiter = claim(fixture, key, digest(50), "domain", "dead", 2);
    REQUIRE(live_waiter.disposition == reusable_claim_disposition::existing_in_flight);
    REQUIRE(live_waiter.token.kind == reusable_reference_kind::waiter);

    auto dead_sweep = janitor_reusable_registry(
        fixture.attachment,
        1'000,
        k_maximum_reusable_janitor_batch,
        reusable_janitor_callbacks{
            .owner_alive = janitor_producers_are_dead_references_are_alive,
        });
    REQUIRE(dead_sweep.error == reusable_registry_error::none);
    REQUIRE(dead_sweep.failed_dead_producers == 1);
    REQUIRE(dead_sweep.retired_failed == 0);
    REQUIRE(dead_sweep.retired_manifest_count == 0);
    REQUIRE(dead_sweep.affected_reference_count == 1);
    REQUIRE(dead_sweep.affected_references[0] == dead.reference);
    const auto failed = snapshot_reusable_generation(fixture.attachment, dead.reference);
    REQUIRE(failed.has_value());
    REQUIRE(failed->state == cache_entry_state::failed);
    REQUIRE(failed->waiter_count == 1);
    REQUIRE(release_reusable_reference(fixture.attachment, live_waiter.token) ==
            reusable_registry_error::none);

    const auto failed_retirement = janitor_reusable_registry(fixture.attachment, 1'001);
    REQUIRE(failed_retirement.error == reusable_registry_error::none);
    REQUIRE(failed_retirement.retired_failed == 1);
    REQUIRE(failed_retirement.affected_reference_count == 1);
    REQUIRE(failed_retirement.affected_references[0] == dead.reference);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
    REQUIRE(release_and_finalize_one_retired(fixture) == reusable_registry_error::none);

    auto expiring = claim(fixture, key, digest(51), "domain", "expiring", 2, 100, 1);
    REQUIRE(expiring);
    const auto expiring_manifest = publish(fixture, expiring.reference, 200);
    REQUIRE(static_cast<bool>(expiring_manifest));
    auto expiry_sweep = janitor_reusable_registry(fixture.attachment, 2'000'000);
    REQUIRE(expiry_sweep.error == reusable_registry_error::none);
    REQUIRE(expiry_sweep.reclaimed_retired == 0);
    REQUIRE(expiry_sweep.retired_expired == 1);
    REQUIRE(expiry_sweep.retired_manifest_count == 1);
    REQUIRE(expiry_sweep.retired_manifests[0].id == expiring.reference.id);
    REQUIRE(expiry_sweep.affected_reference_count == 1);
    REQUIRE(expiry_sweep.affected_references[0] == expiring.reference);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
    REQUIRE(release_and_finalize_one_retired(fixture) == reusable_registry_error::none);

    auto final_sweep = janitor_reusable_registry(fixture.attachment, 2'000'001);
    REQUIRE(final_sweep.error == reusable_registry_error::none);
    REQUIRE(final_sweep.reclaimed_retired == 0);
    const auto snapshot = reusable_registry_snapshot(fixture.attachment);
    REQUIRE(snapshot.indexed_generations == 0);
    REQUIRE(snapshot.retired_generations == 0);
    REQUIRE(snapshot.free_generations == 4);
    REQUIRE(snapshot.arena_free_bytes == 16U << 10);
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}

TEST_CASE("janitor wake references are bounded and deduplicated",
          "[component:cache][layer:unit][feature:registry]") {
    registry_fixture fixture{8, 32U << 10, 1};
    const auto key = coverage_key(52);
    std::array<reusable_generation_reference, 4> references{};
    for (std::size_t index = 0; index < references.size(); ++index) {
        const std::string generation = "dead-" + std::to_string(index);
        auto claimed = claim(fixture,
                             key,
                             digest(static_cast<std::uint8_t>(60 + index)),
                             "domain",
                             generation,
                             static_cast<std::uint32_t>(index + 1));
        REQUIRE(claimed);
        REQUIRE(begin_reusable_build(fixture.attachment, claimed.reference) ==
                reusable_registry_error::none);
        references[index] = claimed.reference;
    }

    constexpr std::uint32_t maximum = 3;
    const auto sweep = janitor_reusable_registry(fixture.attachment,
                                                 1'000,
                                                 maximum,
                                                 reusable_janitor_callbacks{
                                                     .owner_alive = janitor_owner_is_dead,
                                                 });
    REQUIRE(sweep.error == reusable_registry_error::none);
    REQUIRE(sweep.failed_dead_producers == maximum);
    REQUIRE(sweep.retired_failed == maximum);
    REQUIRE(sweep.affected_reference_count == maximum);
    REQUIRE(sweep.affected_reference_count <= sweep.affected_references.size());
    for (std::size_t index = 0; index < sweep.affected_reference_count; ++index) {
        REQUIRE(std::find(references.begin(), references.end(), sweep.affected_references[index]) !=
                references.end());
        for (std::size_t prior = 0; prior < index; ++prior) {
            REQUIRE(sweep.affected_references[prior] != sweep.affected_references[index]);
        }
    }
    REQUIRE(validate_reusable_registry(fixture.attachment) == reusable_registry_error::none);
}
