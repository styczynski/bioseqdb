#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

import bioseq.cache.core;
import bioseq.cache.pg.reusable_planning;

using namespace bioseq::cache;
using namespace bioseq::cache::pg::reuse;

namespace {

BioseqCacheColumnSchemaV2 schema_column(
    std::uint32_t attno,
    std::uint64_t semantic_id) {
    return BioseqCacheColumnSchemaV2{
        .struct_size = sizeof(BioseqCacheColumnSchemaV2),
        .attno = attno,
        .semantic_id_high = 0x1234,
        .semantic_id_low = semantic_id,
        .type_oid = 23,
        .typmod = -1,
        .collation_oid = 0,
        .flags = 0,
    };
}

struct ReleaseCounter {
    std::size_t calls = 0;
    CacheEntryId last{};
    CandidateReferenceKindV1 kind = CandidateReferenceKindV1::None;
};

void count_release(
    void* opaque,
    const CacheEntryId& entry_id,
    CandidateReferenceKindV1 kind) noexcept {
    auto& counter = *static_cast<ReleaseCounter*>(opaque);
    ++counter.calls;
    counter.last = entry_id;
    counter.kind = kind;
}

struct ReactivatingReleaseCounter {
    PlannerCandidateCleanupRecordV1* record = nullptr;
    CacheEntryId replacement{};
    std::array<CandidateReferenceKindV1, 2> kinds{};
    std::size_t calls = 0;
    bool reactivation_succeeded = false;
};

void reactivate_once_release(
    void* opaque,
    const CacheEntryId&,
    CandidateReferenceKindV1 kind) noexcept {
    auto& counter = *static_cast<ReactivatingReleaseCounter*>(opaque);
    counter.kinds[counter.calls++] = kind;
    if (counter.calls == 1) {
        counter.reactivation_succeeded = activate_candidate_reference(
            *counter.record,
            counter.replacement,
            CandidateReferenceKindV1::InFlightWaiter,
            &reactivate_once_release,
            &counter);
    }
}

[[nodiscard]] CoverageSecurityNamespaceV1 security_namespace() noexcept {
    CoverageSecurityNamespaceV1 result{
        .database_oid = 0x01020304U,
        .current_role_oid = 0x11121314U,
        .session_user_oid = 0x21222324U,
        .security_context_flags = 0x31323334U,
        .row_security_mode = 1,
    };
    for (std::size_t index = 0;
         index < result.provider_security_digest.bytes.size();
         ++index) {
        result.provider_security_digest.bytes[index] =
            static_cast<std::uint8_t>(index);
    }
    return result;
}

} // namespace

TEST_CASE("coverage canonical keys frame every security field") {
    const CoverageSecurityNamespaceV1 security = security_namespace();
    const std::array provider_bytes{
        std::byte{0xa1}, std::byte{0xb2}, std::byte{0xc3}};
    std::array<std::byte, kCoverageCanonicalKeyFixedBytesV1 + 3> encoded{};

    const auto size = write_coverage_canonical_key(
        CoverageCanonicalKeyKindV1::Domain,
        security,
        provider_bytes,
        encoded);
    REQUIRE(size.has_value());
    REQUIRE(*size == encoded.size());

    const std::array<std::byte, kCoverageCanonicalKeyFixedBytesV1 + 3>
        expected{
            std::byte{0x42}, std::byte{0x43}, std::byte{0x4b},
            std::byte{0x31}, std::byte{0x01}, std::byte{0x01},
            std::byte{0x02}, std::byte{0x01}, std::byte{0x00},
            std::byte{0x00}, std::byte{0x00}, std::byte{0x10},
            std::byte{0x04}, std::byte{0x03}, std::byte{0x02},
            std::byte{0x01}, std::byte{0x11}, std::byte{0x14},
            std::byte{0x13}, std::byte{0x12}, std::byte{0x11},
            std::byte{0x12}, std::byte{0x24}, std::byte{0x23},
            std::byte{0x22}, std::byte{0x21}, std::byte{0x13},
            std::byte{0x34}, std::byte{0x33}, std::byte{0x32},
            std::byte{0x31}, std::byte{0x14}, std::byte{0x01},
            std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x15}, std::byte{0x00}, std::byte{0x01},
            std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
            std::byte{0x05}, std::byte{0x06}, std::byte{0x07},
            std::byte{0x08}, std::byte{0x09}, std::byte{0x0a},
            std::byte{0x0b}, std::byte{0x0c}, std::byte{0x0d},
            std::byte{0x0e}, std::byte{0x0f}, std::byte{0x10},
            std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
            std::byte{0x14}, std::byte{0x15}, std::byte{0x16},
            std::byte{0x17}, std::byte{0x18}, std::byte{0x19},
            std::byte{0x1a}, std::byte{0x1b}, std::byte{0x1c},
            std::byte{0x1d}, std::byte{0x1e}, std::byte{0x1f},
            std::byte{0x20}, std::byte{0x03}, std::byte{0x00},
            std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
            std::byte{0xa1}, std::byte{0xb2}, std::byte{0xc3},
        };
    REQUIRE(encoded == expected);

    auto generation = encoded;
    REQUIRE(write_coverage_canonical_key(
        CoverageCanonicalKeyKindV1::Generation,
        security,
        provider_bytes,
        generation).has_value());
    REQUIRE(generation != encoded);
    REQUIRE(generation[5] == std::byte{0x02});

    auto changed_security = security;
    changed_security.provider_security_digest =
        kProviderIndependentSecurityDigestV1;
    std::array<std::byte, kCoverageCanonicalKeyFixedBytesV1 + 3>
        provider_independent{};
    REQUIRE(write_coverage_canonical_key(
        CoverageCanonicalKeyKindV1::Domain,
        changed_security,
        provider_bytes,
        provider_independent).has_value());
    REQUIRE(provider_independent != encoded);
    REQUIRE(std::all_of(
        provider_independent.begin() + 37,
        provider_independent.begin() + 69,
        [](std::byte value) noexcept { return value == std::byte{0}; }));
}

TEST_CASE("coverage canonical keys reject omitted or malformed namespaces") {
    const std::array provider_bytes{std::byte{0x55}};
    std::array<std::byte, kCoverageCanonicalKeyFixedBytesV1 + 1> output{};
    const auto valid = security_namespace();

    auto require_namespace_error =
        [&output, &provider_bytes](CoverageSecurityNamespaceV1 value) {
        const auto result = write_coverage_canonical_key(
            CoverageCanonicalKeyKindV1::Domain,
            value,
            provider_bytes,
            output);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(
            result.error()
            == CoverageCanonicalKeyErrorV1::InvalidSecurityNamespace);
    };

    auto invalid = valid;
    invalid.database_oid = 0;
    require_namespace_error(invalid);
    invalid = valid;
    invalid.current_role_oid = 0;
    require_namespace_error(invalid);
    invalid = valid;
    invalid.session_user_oid = 0;
    require_namespace_error(invalid);
    invalid = valid;
    invalid.row_security_mode = -1;
    require_namespace_error(invalid);
    invalid.row_security_mode = 2;
    require_namespace_error(invalid);
    invalid = valid;
    invalid.reserved = 1;
    require_namespace_error(invalid);

    output.fill(std::byte{0x7f});
    const auto short_output = write_coverage_canonical_key(
        CoverageCanonicalKeyKindV1::Domain,
        valid,
        provider_bytes,
        std::span<std::byte>{output}.first(output.size() - 1));
    REQUIRE_FALSE(short_output.has_value());
    REQUIRE(
        short_output.error()
        == CoverageCanonicalKeyErrorV1::OutputTooSmall);
    REQUIRE(std::all_of(
        output.begin(),
        output.end(),
        [](std::byte value) noexcept { return value == std::byte{0x7f}; }));
}

TEST_CASE("coverage discovery collision-checks and bounds candidate buckets") {
    const std::array<std::byte, 3> requested_key{
        std::byte{1}, std::byte{2}, std::byte{3}};
    const std::array<std::byte, 3> collision_key{
        std::byte{1}, std::byte{2}, std::byte{4}};
    const std::array<std::byte, 2> generation_key{
        std::byte{9}, std::byte{7}};

    CacheDigest256 domain_digest{};
    domain_digest.bytes[0] = 44;
    CoverageSecurityNamespaceV1 security{};
    security.database_oid = 17;
    security.current_role_oid = 18;
    security.session_user_oid = 19;

    const CoverageCandidateProbeV1 probe{
        .domain_digest = domain_digest,
        .canonical_domain_key = requested_key,
        .security_namespace = security,
        .provider_id = 7,
        .compatibility_family_id = 11,
        .required_capability_mask = 0b0011,
    };

    std::vector<CoverageCandidateViewV1> candidates(70);
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        candidates[index] = CoverageCandidateViewV1{
            .entry_id = {
                .generation = index + 1,
                .claim_sequence = 100 + index,
            },
            .domain_digest = domain_digest,
            .canonical_domain_key = requested_key,
            .canonical_generation_key = generation_key,
            .security_namespace = security,
            .provider_id = 7,
            .compatibility_family_id = 11,
            .capability_mask = 0b0111,
            .complete = index % 2 == 0,
            .in_flight = index % 2 != 0,
            .estimated_covered_rows = 10'000 - index,
            .stored_bytes = 1'000 + index,
        };
        candidates[index].entry_id.digest.bytes[31] =
            static_cast<std::uint8_t>(index);
    }
    candidates[3].canonical_domain_key = collision_key;
    candidates[4].security_namespace.current_role_oid = 999;
    candidates[5].security_namespace.row_security_mode = 2;

    const auto selected = select_candidates(probe, candidates);
    REQUIRE(selected.visited_count == kMaxCoverageGenerationsPerDomain);
    REQUIRE(selected.bucket_truncated);
    REQUIRE(selected.selected_count == kMaxSourcesPerCoverageNode);
    REQUIRE(selected.result_truncated);
    REQUIRE(selected.digest_collision_count == 1);
    REQUIRE(selected.invalid_namespace_count == 1);
    REQUIRE(selected.indices[0] == 0);
    REQUIRE(selected.indices[1] == 1);

    REQUIRE_FALSE(exact_current_key_equal(
        domain_digest, requested_key, domain_digest, collision_key));
    REQUIRE(exact_current_key_equal(
        domain_digest, requested_key, domain_digest, requested_key));
}

TEST_CASE("candidate ordering is deterministic at every tie breaker") {
    const std::array<std::byte, 1> key{std::byte{1}};
    CoverageCandidateViewV1 complete{
        .canonical_domain_key = key,
        .canonical_generation_key = key,
        .complete = true,
        .estimated_covered_rows = 100,
        .stored_bytes = 50,
    };
    complete.entry_id.claim_sequence = 4;
    CoverageCandidateViewV1 building = complete;
    building.complete = false;
    building.in_flight = true;
    REQUIRE(candidate_precedes(complete, building));

    CoverageCandidateViewV1 denser = complete;
    denser.stored_bytes = 49;
    REQUIRE(candidate_precedes(denser, complete));

    CoverageCandidateViewV1 earlier = complete;
    earlier.entry_id.claim_sequence = 3;
    REQUIRE(candidate_precedes(earlier, complete));

    CoverageCandidateViewV1 digest_first = complete;
    CoverageCandidateViewV1 digest_last = complete;
    digest_first.entry_id.digest.bytes[0] = 1;
    digest_last.entry_id.digest.bytes[0] = 2;
    REQUIRE(candidate_precedes(digest_first, digest_last));
}

TEST_CASE("provider domain and decorated schema contracts are exact") {
    const std::array boundary{
        schema_column(1, 101), schema_column(2, 102)};
    const std::array lineage{schema_column(1, 201)};
    const std::array<std::uint32_t, 2> required_boundary{2, 1};
    const std::array<std::uint32_t, 1> required_lineage{1};

    BioseqCachePathDomainV2 domain{
        .struct_size = sizeof(BioseqCachePathDomainV2),
        .provider_id = 7,
        .compatibility_family_id = 11,
        .capability_mask = 3,
        .coverage_predicate = reinterpret_cast<::Expr*>(0x1),
        .boundary_columns = boundary.data(),
        .boundary_column_count = boundary.size(),
        .lineage_columns = lineage.data(),
        .lineage_column_count = lineage.size(),
        .required_boundary_attnos = required_boundary.data(),
        .required_boundary_attno_count = required_boundary.size(),
        .required_lineage_attnos = required_lineage.data(),
        .required_lineage_attno_count = required_lineage.size(),
        .provider_security_digest_format_version =
            BIOSEQ_CACHE_PROVIDER_SECURITY_DIGEST_FORMAT_V1,
        .provider_security_flags = 0,
    };
    REQUIRE(validate_path_domain(7, &domain).has_value());
    REQUIRE(
        provider_security_digest(domain)
        == kProviderIndependentSecurityDigestV1);

    domain.struct_size = offsetof(
        BioseqCachePathDomainV2,
        provider_security_digest_format_version);
    REQUIRE(
        validate_path_domain(7, &domain).error()
        == ProviderContractErrorV1::StructSize);
    domain.struct_size = sizeof(BioseqCachePathDomainV2);

    BioseqCacheDecoratedPathV2 decorated{
        .struct_size = sizeof(BioseqCacheDecoratedPathV2),
        .flags = 0,
        .path = reinterpret_cast<::Path*>(0x1),
        .actual_boundary_columns = boundary.data(),
        .actual_boundary_column_count = boundary.size(),
        .actual_lineage_columns = lineage.data(),
        .actual_lineage_column_count = lineage.size(),
    };
    REQUIRE(validate_decorated_path(&domain, &decorated).has_value());

    auto permuted = boundary;
    std::swap(permuted[0], permuted[1]);
    decorated.actual_boundary_columns = permuted.data();
    REQUIRE(
        validate_decorated_path(&domain, &decorated).error()
        == ProviderContractErrorV1::DecoratedBoundaryMismatch);

    domain.compatibility_family_id = 0;
    REQUIRE(
        validate_path_domain(7, &domain).error()
        == ProviderContractErrorV1::InvalidCompatibilityFamily);
    domain.compatibility_family_id = 11;
    domain.provider_security_digest_format_version = 0;
    REQUIRE(
        validate_path_domain(7, &domain).error()
        == ProviderContractErrorV1::SecurityNamespace);
    domain.provider_security_digest_format_version =
        BIOSEQ_CACHE_PROVIDER_SECURITY_DIGEST_FORMAT_V1;
    domain.provider_security_flags = 1;
    REQUIRE(
        validate_path_domain(7, &domain).error()
        == ProviderContractErrorV1::SecurityNamespace);
    domain.provider_security_flags = 0;
    domain.provider_security_digest[31] = 0xa5;
    REQUIRE(validate_path_domain(7, &domain).has_value());
    REQUIRE(provider_security_digest(domain).bytes[31] == 0xa5);
}

TEST_CASE("runtime recipe and digest sink reject malformed or oversized data") {
    BioseqCachePlanDigestRecipeV2 recipe{
        .struct_size = sizeof(BioseqCachePlanDigestRecipeV2),
        .flags = 0,
        .digest_format_version = kProviderRecipeFormatVersion,
        .reserved = 0,
        .plan_domain_nodes = reinterpret_cast<::List*>(0x1),
    };
    REQUIRE(validate_plan_digest_recipe(&recipe, 1, 10, 2).has_value());
    recipe.digest_format_version = 99;
    REQUIRE(
        validate_plan_digest_recipe(&recipe, 1, 10, 2).error()
        == ProviderContractErrorV1::RecipeShape);

    std::array<std::byte, 4> storage{};
    BoundedDigestSinkBufferV1 buffer{.bytes = storage};
    auto sink = make_digest_sink(buffer);
    const std::array<std::byte, 3> first{
        std::byte{1}, std::byte{2}, std::byte{3}};
    REQUIRE(sink.append(sink.opaque, first.data(), first.size()));
    REQUIRE_FALSE(sink.append(sink.opaque, first.data(), first.size()));
    REQUIRE(buffer.failed);
}

TEST_CASE("finalized recipe and expected digest are accepted atomically") {
    CacheDigest256 digest_a{};
    digest_a.bytes[0] = 0xa1;
    CacheDigest256 digest_b{};
    digest_b.bytes[0] = 0xb2;

    BioseqCachePlanDigestRecipeV2 recipe{
        .struct_size = sizeof(BioseqCachePlanDigestRecipeV2),
        .flags = 0,
        .digest_format_version = kProviderRecipeFormatVersion,
        .reserved = 0,
        .plan_domain_nodes = reinterpret_cast<::List*>(0x1),
    };
    std::copy(
        digest_a.bytes.begin(),
        digest_a.bytes.end(),
        recipe.expected_generation_digest);

    const auto finalized_a = finalize_plan_digest_recipe(
        &recipe, 1, 37, 4, &digest_a);
    REQUIRE(finalized_a.has_value());
    REQUIRE(finalized_a->expected_generation_digest == digest_a);
    REQUIRE(finalized_a->digest_format_version
            == kProviderRecipeFormatVersion);
    REQUIRE(finalized_a->recipe_count == 1);
    REQUIRE(finalized_a->serialized_bytes == 37);
    REQUIRE(finalized_a->node_count == 4);

    std::copy(
        digest_b.bytes.begin(),
        digest_b.bytes.end(),
        recipe.expected_generation_digest);
    recipe.plan_domain_nodes = nullptr;
    const auto partial_output = finalize_plan_digest_recipe(
        &recipe, 1, 37, 4, &digest_a);
    REQUIRE_FALSE(partial_output.has_value());
    REQUIRE(partial_output.error() == ProviderContractErrorV1::RecipeShape);
    REQUIRE(finalized_a->expected_generation_digest == digest_a);

    recipe.plan_domain_nodes = reinterpret_cast<::List*>(0x1);
    const auto path_final_mismatch = finalize_plan_digest_recipe(
        &recipe, 1, 37, 4, &digest_a);
    REQUIRE_FALSE(path_final_mismatch.has_value());
    REQUIRE(
        path_final_mismatch.error()
        == ProviderContractErrorV1::CoverageDigestMismatch);
    REQUIRE(finalized_a->expected_generation_digest == digest_a);

    const auto exact_b = finalize_plan_digest_recipe(
        &recipe, 1, 37, 4);
    REQUIRE(exact_b.has_value());
    REQUIRE(exact_b->expected_generation_digest == digest_b);
}

TEST_CASE("branch ownership is occurrence-disjoint and SQL NULL correct") {
    const std::array<bool, 3> valid{true, false, true};
    const std::array predicates{
        SqlBool::Unknown, SqlBool::TrueValue, SqlBool::TrueValue};
    const auto selected = choose_coverage_branch(
        SqlBool::TrueValue, predicates, valid);
    REQUIRE(selected.kind == CoverageBranchKindV1::Cached);
    REQUIRE(selected.source_index == 2);
    REQUIRE(source_owns_occurrence(
        2, SqlBool::TrueValue, predicates, valid));
    REQUIRE_FALSE(source_owns_occurrence(
        0, SqlBool::TrueValue, predicates, valid));

    const std::array no_match{
        SqlBool::Unknown, SqlBool::FalseValue, SqlBool::Unknown};
    REQUIRE(
        choose_coverage_branch(SqlBool::TrueValue, no_match, valid).kind
        == CoverageBranchKindV1::Residual);
    REQUIRE(
        choose_coverage_branch(SqlBool::Unknown, predicates, valid).kind
        == CoverageBranchKindV1::Drop);
}

TEST_CASE("late fetch sink enforces one value and one terminal per reference") {
    std::array<std::uint8_t, 2> terminal{};
    std::array<std::uint8_t, 4> seen{};
    std::array<std::uintptr_t, 4> values{};
    std::array<std::uint8_t, 4> nulls{};
    LateFetchSinkStateV1 state{
        .reference_count = 2,
        .attribute_count = 2,
        .terminal_status = terminal,
        .value_seen = seen,
        .values = values,
        .nulls = nulls,
    };
    REQUIRE(initialize_late_fetch_sink_state(state));
    auto sink = make_late_fetch_sink(state);
    for (std::size_t reference = 0; reference < 2; ++reference) {
        REQUIRE(
            sink.emit_value(
                sink.opaque, reference, 0, 100 + reference, false)
            == BIOSEQ_CACHE_STATUS_OK);
        REQUIRE(
            sink.emit_value(
                sink.opaque, reference, 1, 0, true)
            == BIOSEQ_CACHE_STATUS_OK);
        sink.emit_reference_status(
            sink.opaque, reference, BIOSEQ_CACHE_STATUS_OK);
    }
    REQUIRE(late_fetch_complete_and_valid(state));

    REQUIRE(
        sink.emit_value(sink.opaque, 0, 0, 1, false)
        == BIOSEQ_CACHE_STATUS_ERROR);
    REQUIRE_FALSE(late_fetch_complete_and_valid(state));
}

TEST_CASE("candidate cleanup exhausts reference kinds and reentrancy") {
    ReleaseCounter counter{};
    CacheEntryId entry{};
    entry.generation = 9;
    entry.claim_sequence = 13;
    constexpr std::array kinds{
        CandidateReferenceKindV1::ProvisionalPin,
        CandidateReferenceKindV1::InFlightWaiter,
        CandidateReferenceKindV1::AdmissionReservation,
    };
    for (const CandidateReferenceKindV1 kind : kinds) {
        PlannerCandidateCleanupRecordV1 next{};
        PlannerCandidateCleanupRecordV1 cleanup{.next = &next};
        release_candidate_reference_noerror(cleanup);
        REQUIRE_FALSE(activate_candidate_reference(
            cleanup,
            entry,
            CandidateReferenceKindV1::None,
            &count_release,
            &counter));
        REQUIRE_FALSE(activate_candidate_reference(
            cleanup, entry, kind, nullptr, &counter));
        REQUIRE_FALSE(cleanup.active);
        REQUIRE(cleanup.next == &next);
        REQUIRE(cleanup.reference_kind == CandidateReferenceKindV1::None);
        REQUIRE(activate_candidate_reference(
            cleanup, entry, kind, &count_release, &counter));
        REQUIRE_FALSE(activate_candidate_reference(
            cleanup, entry, kind, &count_release, &counter));
        release_candidate_reference_noerror(cleanup);
        release_candidate_reference_noerror(cleanup);
        REQUIRE_FALSE(cleanup.active);
        REQUIRE(cleanup.reference_kind == CandidateReferenceKindV1::None);
        REQUIRE(cleanup.release == nullptr);
        REQUIRE(cleanup.release_opaque == nullptr);
        REQUIRE(cleanup.next == &next);
        REQUIRE(counter.kind == kind);
    }
    REQUIRE(counter.calls == kinds.size());
    REQUIRE(counter.last.generation == 9);
    REQUIRE(counter.last.claim_sequence == 13);

    PlannerCandidateCleanupRecordV1 reentrant{};
    ReactivatingReleaseCounter reactivating{
        .record = &reentrant,
        .replacement = {
            .generation = 10,
            .claim_sequence = 14,
        },
    };
    REQUIRE(activate_candidate_reference(
        reentrant,
        entry,
        CandidateReferenceKindV1::ProvisionalPin,
        &reactivate_once_release,
        &reactivating));
    release_candidate_reference_noerror(reentrant);
    REQUIRE(reactivating.reactivation_succeeded);
    REQUIRE(reentrant.active);
    REQUIRE(
        reentrant.reference_kind
        == CandidateReferenceKindV1::InFlightWaiter);
    release_candidate_reference_noerror(reentrant);
    REQUIRE_FALSE(reentrant.active);
    REQUIRE(reactivating.calls == 2);
    REQUIRE(
        reactivating.kinds[0]
        == CandidateReferenceKindV1::ProvisionalPin);
    REQUIRE(
        reactivating.kinds[1]
        == CandidateReferenceKindV1::InFlightWaiter);
}

TEST_CASE("gate publication is one-way and exhaustive at its bounds") {
    ReuseGateFreezeV1 empty{};
    REQUIRE_FALSE(source_gate_is_valid(empty, 0));
    REQUIRE_FALSE(begin_gate_preflight(empty, 0));
    REQUIRE_FALSE(begin_gate_preflight(
        empty, kMaxSourcesPerCoverageNode + 1));
    REQUIRE(empty.state == ReusePreflightStateV1::Empty);

    ReuseGateFreezeV1 maximum{};
    REQUIRE(begin_gate_preflight(maximum, kMaxSourcesPerCoverageNode));
    REQUIRE(stage_valid_source(maximum, 0));
    REQUIRE(stage_valid_source(
        maximum, kMaxSourcesPerCoverageNode - 1));
    REQUIRE_FALSE(stage_valid_source(
        maximum, kMaxSourcesPerCoverageNode));
    REQUIRE_FALSE(source_gate_is_valid(maximum, 0));
    REQUIRE(freeze_source_decisions(maximum));
    REQUIRE(source_gate_is_valid(maximum, 0));
    REQUIRE(source_gate_is_valid(
        maximum, kMaxSourcesPerCoverageNode - 1));
    REQUIRE_FALSE(source_gate_is_valid(
        maximum, kMaxSourcesPerCoverageNode));
    REQUIRE_FALSE(freeze_source_decisions(maximum));
    REQUIRE_FALSE(stage_valid_source(maximum, 1));
    REQUIRE_FALSE(begin_gate_preflight(maximum, 1));
    freeze_all_sources_invalid(maximum);
    REQUIRE(source_gate_is_valid(maximum, 0));

    ReuseGateFreezeV1 fail_open{};
    REQUIRE(begin_gate_preflight(fail_open, 3));
    REQUIRE(stage_valid_source(fail_open, 0));
    REQUIRE(stage_valid_source(fail_open, 2));
    freeze_all_sources_invalid(fail_open);
    REQUIRE(fail_open.state == ReusePreflightStateV1::Frozen);
    for (std::size_t index = 0; index < 3; ++index) {
        REQUIRE_FALSE(source_gate_is_valid(fail_open, index));
    }
}

TEST_CASE("replacement plans retain independent frozen digests and gates") {
    CacheDigest256 digest_a{};
    digest_a.bytes[0] = 0xa1;
    CacheDigest256 digest_b{};
    digest_b.bytes[0] = 0xb2;
    CacheDigest256 live_configuration_digest = digest_a;
    BioseqCachePlanDigestRecipeV2 recipe{
        .struct_size = sizeof(BioseqCachePlanDigestRecipeV2),
        .digest_format_version = kProviderRecipeFormatVersion,
        .plan_domain_nodes = reinterpret_cast<::List*>(0x1),
    };

    std::copy(
        live_configuration_digest.bytes.begin(),
        live_configuration_digest.bytes.end(),
        recipe.expected_generation_digest);
    const auto plan_a = finalize_plan_digest_recipe(
        &recipe, 1, 8, 1, &live_configuration_digest);
    REQUIRE(plan_a.has_value());
    const CacheDigest256 paused_a_begin_digest =
        plan_a->expected_generation_digest;

    ReuseGateFreezeV1 plan_a_gates{};
    REQUIRE(begin_gate_preflight(plan_a_gates, 2));
    REQUIRE(stage_valid_source(plan_a_gates, 0));
    REQUIRE(freeze_source_decisions(plan_a_gates));

    live_configuration_digest = digest_b;
    std::copy(
        live_configuration_digest.bytes.begin(),
        live_configuration_digest.bytes.end(),
        recipe.expected_generation_digest);
    const auto plan_b = finalize_plan_digest_recipe(
        &recipe, 1, 8, 1, &live_configuration_digest);
    REQUIRE(plan_b.has_value());
    const CacheDigest256 replacement_b_begin_digest =
        plan_b->expected_generation_digest;
    ReuseGateFreezeV1 plan_b_gates{};
    REQUIRE(begin_gate_preflight(plan_b_gates, 2));
    REQUIRE(stage_valid_source(plan_b_gates, 1));
    REQUIRE(freeze_source_decisions(plan_b_gates));

    REQUIRE(runtime_digest_matches(
        plan_a->expected_generation_digest,
        paused_a_begin_digest));
    REQUIRE(runtime_digest_matches(
        plan_b->expected_generation_digest,
        replacement_b_begin_digest));
    REQUIRE_FALSE(runtime_digest_matches(
        plan_a->expected_generation_digest,
        live_configuration_digest));
    REQUIRE(runtime_digest_matches(
        plan_b->expected_generation_digest,
        live_configuration_digest));
    REQUIRE(source_gate_is_valid(plan_a_gates, 0));
    REQUIRE_FALSE(source_gate_is_valid(plan_a_gates, 1));
    REQUIRE_FALSE(source_gate_is_valid(plan_b_gates, 0));
    REQUIRE(source_gate_is_valid(plan_b_gates, 1));

    REQUIRE_FALSE(begin_gate_preflight(plan_a_gates, 2));
    freeze_all_sources_invalid(plan_a_gates);
    REQUIRE(source_gate_is_valid(plan_a_gates, 0));
    REQUIRE_FALSE(source_gate_is_valid(plan_a_gates, 1));
}

TEST_CASE("reuse waits obey descending owned claim order") {
    const std::array<std::uint64_t, 3> owned{90, 70, 50};
    REQUIRE(claim_order_allows_wait(49, owned));
    REQUIRE_FALSE(claim_order_allows_wait(50, owned));
    REQUIRE_FALSE(claim_order_allows_wait(91, owned));
    REQUIRE_FALSE(claim_order_allows_wait(0, owned));
}

TEST_CASE("fragment binding semantics ignore raw RTI and separate occurrences") {
    CoverageFragmentBindingSignatureV1 first{
        .provider_rte_occurrence_ordinal = 0,
        .provider_rte_occurrence_count = 2,
        .logical_relation_oid = 4100,
        .selected_relation_oid = 4101,
        .provider_id = 77,
    };
    first.binding_role_digest.bytes[0] = 0xa5;
    CoverageFragmentBindingSignatureV1 second = first;
    second.provider_rte_occurrence_ordinal = 1;
    REQUIRE(valid_coverage_fragment_binding(first));
    REQUIRE(valid_coverage_fragment_binding(second));

    const std::array<std::byte, 3> predicate{
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
    std::array<std::byte, 256> first_bytes{};
    std::array<std::byte, 256> second_bytes{};
    const auto first_size = write_coverage_binding_semantics(
        CoverageCanonicalKeyKindV1::Generation,
        first,
        predicate,
        first_bytes);
    const auto second_size = write_coverage_binding_semantics(
        CoverageCanonicalKeyKindV1::Generation,
        second,
        predicate,
        second_bytes);
    REQUIRE(first_size.has_value());
    REQUIRE(second_size.has_value());
    REQUIRE(*first_size == *second_size);
    REQUIRE_FALSE(std::equal(
        first_bytes.begin(), first_bytes.begin() + *first_size,
        second_bytes.begin()));

    first.provider_rte_occurrence_ordinal = 2;
    REQUIRE_FALSE(valid_coverage_fragment_binding(first));
}
