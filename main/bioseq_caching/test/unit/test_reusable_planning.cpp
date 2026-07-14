#include <catch2/catch_test_macros.hpp>

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

BioseqCacheColumnSchemaV1 schema_column(
    std::uint32_t attno,
    std::uint64_t semantic_id) {
    return BioseqCacheColumnSchemaV1{
        .struct_size = sizeof(BioseqCacheColumnSchemaV1),
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

} // namespace

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

    const auto selected = select_candidates(probe, candidates);
    REQUIRE(selected.visited_count == kMaxCoverageGenerationsPerDomain);
    REQUIRE(selected.bucket_truncated);
    REQUIRE(selected.selected_count == kMaxSourcesPerCoverageNode);
    REQUIRE(selected.result_truncated);
    REQUIRE(selected.digest_collision_count == 1);
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

    BioseqCachePathDomainV1 domain{
        .struct_size = sizeof(BioseqCachePathDomainV1),
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
    };
    REQUIRE(validate_path_domain(7, &domain).has_value());

    BioseqCacheDecoratedPathV1 decorated{
        .struct_size = sizeof(BioseqCacheDecoratedPathV1),
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
}

TEST_CASE("runtime recipe and digest sink reject malformed or oversized data") {
    BioseqCachePlanDigestRecipeV1 recipe{
        .struct_size = sizeof(BioseqCachePlanDigestRecipeV1),
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

TEST_CASE("candidate cleanup and gate publication remain idempotent") {
    ReleaseCounter counter{};
    CacheEntryId entry{};
    entry.generation = 9;
    PlannerCandidateCleanupRecordV1 cleanup{};
    REQUIRE(activate_candidate_reference(
        cleanup,
        entry,
        CandidateReferenceKindV1::ProvisionalPin,
        &count_release,
        &counter));
    REQUIRE_FALSE(activate_candidate_reference(
        cleanup,
        entry,
        CandidateReferenceKindV1::ProvisionalPin,
        &count_release,
        &counter));
    release_candidate_reference_noerror(cleanup);
    release_candidate_reference_noerror(cleanup);
    REQUIRE(counter.calls == 1);
    REQUIRE(counter.last.generation == 9);
    REQUIRE(counter.kind == CandidateReferenceKindV1::ProvisionalPin);

    ReuseGateFreezeV1 gates{};
    REQUIRE(begin_gate_preflight(gates, 3));
    REQUIRE(stage_valid_source(gates, 1));
    REQUIRE_FALSE(source_gate_is_valid(gates, 1));
    REQUIRE(freeze_source_decisions(gates));
    REQUIRE(source_gate_is_valid(gates, 1));
    REQUIRE_FALSE(source_gate_is_valid(gates, 0));
    REQUIRE_FALSE(stage_valid_source(gates, 2));
}

TEST_CASE("reuse waits obey descending owned claim order") {
    const std::array<std::uint64_t, 3> owned{90, 70, 50};
    REQUIRE(claim_order_allows_wait(49, owned));
    REQUIRE_FALSE(claim_order_allows_wait(50, owned));
    REQUIRE_FALSE(claim_order_allows_wait(91, owned));
    REQUIRE_FALSE(claim_order_allows_wait(0, owned));
}
