#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

import bioseq.cache.core;

using namespace bioseq::cache;
using namespace bioseq::cache::pg::reuse;

namespace {

bioseq_cache_column_schema schema_column(std::uint32_t attno, std::uint64_t semantic_id) {
    return bioseq_cache_column_schema{
        .struct_size = sizeof(bioseq_cache_column_schema),
        .attno = attno,
        .semantic_id_high = 0x1234,
        .semantic_id_low = semantic_id,
        .type_oid = 23,
        .typmod = -1,
        .collation_oid = 0,
        .flags = 0,
    };
}

struct release_counter {
    std::size_t calls = 0;
    cache_entry_id last{};
    candidate_reference_kind kind = candidate_reference_kind::none;
};

void count_release(void* opaque,
                   const cache_entry_id& entry_id,
                   candidate_reference_kind kind) noexcept {
    auto& counter = *static_cast<release_counter*>(opaque);
    ++counter.calls;
    counter.last = entry_id;
    counter.kind = kind;
}

struct reactivating_release_counter {
    planner_candidate_cleanup_record* record = nullptr;
    cache_entry_id replacement{};
    std::array<candidate_reference_kind, 2> kinds{};
    std::size_t calls = 0;
    bool reactivation_succeeded = false;
};

void reactivate_once_release(void* opaque,
                             const cache_entry_id&,
                             candidate_reference_kind kind) noexcept {
    auto& counter = *static_cast<reactivating_release_counter*>(opaque);
    counter.kinds[counter.calls++] = kind;
    if (counter.calls == 1) {
        counter.reactivation_succeeded =
            activate_candidate_reference(*counter.record,
                                         counter.replacement,
                                         candidate_reference_kind::in_flight_waiter,
                                         &reactivate_once_release,
                                         &counter);
    }
}

[[nodiscard]] coverage_security_namespace security_namespace() noexcept {
    coverage_security_namespace result{
        .database_oid = 0x01020304U,
        .current_role_oid = 0x11121314U,
        .session_user_oid = 0x21222324U,
        .security_context_flags = 0x31323334U,
        .row_security_mode = 1,
    };
    for (std::size_t index = 0; index < result.provider_security_digest.bytes.size(); ++index) {
        result.provider_security_digest.bytes[index] = static_cast<std::uint8_t>(index);
    }
    return result;
}

} // namespace

TEST_CASE("coverage canonical keys frame every security field",
          "[component:cache][layer:unit][feature:provider]") {
    const coverage_security_namespace security = security_namespace();
    const std::array provider_bytes{std::byte{0xa1}, std::byte{0xb2}, std::byte{0xc3}};
    std::array<std::byte, k_coverage_canonical_key_fixed_bytes + 3> encoded{};

    const auto size = write_coverage_canonical_key(
        coverage_canonical_key_kind::domain, security, provider_bytes, encoded);
    REQUIRE(size.has_value());
    REQUIRE(*size == encoded.size());

    const std::array<std::byte, k_coverage_canonical_key_fixed_bytes + 3> expected{
        std::byte{0x42}, std::byte{0x43}, std::byte{0x4b}, std::byte{0x31}, std::byte{0x01},
        std::byte{0x01}, std::byte{0x02}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x10}, std::byte{0x04}, std::byte{0x03}, std::byte{0x02},
        std::byte{0x01}, std::byte{0x11}, std::byte{0x14}, std::byte{0x13}, std::byte{0x12},
        std::byte{0x11}, std::byte{0x12}, std::byte{0x24}, std::byte{0x23}, std::byte{0x22},
        std::byte{0x21}, std::byte{0x13}, std::byte{0x34}, std::byte{0x33}, std::byte{0x32},
        std::byte{0x31}, std::byte{0x14}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x15}, std::byte{0x00}, std::byte{0x01}, std::byte{0x02},
        std::byte{0x03}, std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07},
        std::byte{0x08}, std::byte{0x09}, std::byte{0x0a}, std::byte{0x0b}, std::byte{0x0c},
        std::byte{0x0d}, std::byte{0x0e}, std::byte{0x0f}, std::byte{0x10}, std::byte{0x11},
        std::byte{0x12}, std::byte{0x13}, std::byte{0x14}, std::byte{0x15}, std::byte{0x16},
        std::byte{0x17}, std::byte{0x18}, std::byte{0x19}, std::byte{0x1a}, std::byte{0x1b},
        std::byte{0x1c}, std::byte{0x1d}, std::byte{0x1e}, std::byte{0x1f}, std::byte{0x20},
        std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xa1}, std::byte{0xb2},
        std::byte{0xc3},
    };
    REQUIRE(encoded == expected);

    auto generation = encoded;
    REQUIRE(write_coverage_canonical_key(
                coverage_canonical_key_kind::generation, security, provider_bytes, generation)
                .has_value());
    REQUIRE(generation != encoded);
    REQUIRE(generation[5] == std::byte{0x02});

    auto changed_security = security;
    changed_security.provider_security_digest = k_provider_independent_security_digest;
    std::array<std::byte, k_coverage_canonical_key_fixed_bytes + 3> provider_independent{};
    REQUIRE(write_coverage_canonical_key(coverage_canonical_key_kind::domain,
                                         changed_security,
                                         provider_bytes,
                                         provider_independent)
                .has_value());
    REQUIRE(provider_independent != encoded);
    REQUIRE(std::all_of(provider_independent.begin() + 37,
                        provider_independent.begin() + 69,
                        [](std::byte value) noexcept { return value == std::byte{0}; }));
}

TEST_CASE("coverage canonical keys reject omitted or malformed namespaces",
          "[component:cache][layer:unit][feature:provider]") {
    const std::array provider_bytes{std::byte{0x55}};
    std::array<std::byte, k_coverage_canonical_key_fixed_bytes + 1> output{};
    const auto valid = security_namespace();

    auto require_namespace_error = [&output, &provider_bytes](coverage_security_namespace value) {
        const auto result = write_coverage_canonical_key(
            coverage_canonical_key_kind::domain, value, provider_bytes, output);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error() == coverage_canonical_key_error::invalid_security_namespace);
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
    const auto short_output =
        write_coverage_canonical_key(coverage_canonical_key_kind::domain,
                                     valid,
                                     provider_bytes,
                                     std::span<std::byte>{output}.first(output.size() - 1));
    REQUIRE_FALSE(short_output.has_value());
    REQUIRE(short_output.error() == coverage_canonical_key_error::output_too_small);
    REQUIRE(std::all_of(output.begin(), output.end(), [](std::byte value) noexcept {
        return value == std::byte{0x7f};
    }));
}

TEST_CASE("coverage discovery collision-checks and bounds candidate buckets",
          "[component:cache][layer:unit][feature:provider]") {
    const std::array<std::byte, 3> requested_key{std::byte{1}, std::byte{2}, std::byte{3}};
    const std::array<std::byte, 3> collision_key{std::byte{1}, std::byte{2}, std::byte{4}};
    const std::array<std::byte, 2> generation_key{std::byte{9}, std::byte{7}};

    cache_digest_256 domain_digest{};
    domain_digest.bytes[0] = 44;
    coverage_security_namespace security{};
    security.database_oid = 17;
    security.current_role_oid = 18;
    security.session_user_oid = 19;

    const coverage_candidate_probe probe{
        .domain_digest = domain_digest,
        .canonical_domain_key = requested_key,
        .security_namespace = security,
        .provider_id = 7,
        .compatibility_family_id = 11,
        .required_capability_mask = 0b0011,
    };

    std::vector<coverage_candidate_view> candidates(70);
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        candidates[index] = coverage_candidate_view{
            .entry_id =
                {
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
        candidates[index].entry_id.digest.bytes[31] = static_cast<std::uint8_t>(index);
    }
    candidates[3].canonical_domain_key = collision_key;
    candidates[4].security_namespace.current_role_oid = 999;
    candidates[5].security_namespace.row_security_mode = 2;

    const auto selected = select_candidates(probe, candidates);
    REQUIRE(selected.visited_count == k_maximum_coverage_generations_per_domain);
    REQUIRE(selected.bucket_truncated);
    REQUIRE(selected.selected_count == k_maximum_sources_per_coverage_node);
    REQUIRE(selected.result_truncated);
    REQUIRE(selected.digest_collision_count == 1);
    REQUIRE(selected.invalid_namespace_count == 1);
    REQUIRE(selected.indices[0] == 0);
    REQUIRE(selected.indices[1] == 1);

    REQUIRE_FALSE(
        exact_current_key_equal(domain_digest, requested_key, domain_digest, collision_key));
    REQUIRE(exact_current_key_equal(domain_digest, requested_key, domain_digest, requested_key));
}

TEST_CASE("candidate ordering is deterministic at every tie breaker",
          "[component:cache][layer:unit][feature:provider]") {
    const std::array<std::byte, 1> key{std::byte{1}};
    coverage_candidate_view complete{
        .canonical_domain_key = key,
        .canonical_generation_key = key,
        .complete = true,
        .estimated_covered_rows = 100,
        .stored_bytes = 50,
    };
    complete.entry_id.claim_sequence = 4;
    coverage_candidate_view building = complete;
    building.complete = false;
    building.in_flight = true;
    REQUIRE(candidate_precedes(complete, building));

    coverage_candidate_view denser = complete;
    denser.stored_bytes = 49;
    REQUIRE(candidate_precedes(denser, complete));

    coverage_candidate_view earlier = complete;
    earlier.entry_id.claim_sequence = 3;
    REQUIRE(candidate_precedes(earlier, complete));

    coverage_candidate_view digest_first = complete;
    coverage_candidate_view digest_last = complete;
    digest_first.entry_id.digest.bytes[0] = 1;
    digest_last.entry_id.digest.bytes[0] = 2;
    REQUIRE(candidate_precedes(digest_first, digest_last));
}

TEST_CASE("provider domain and decorated schema contracts are exact",
          "[component:cache][layer:unit][feature:provider]") {
    const std::array boundary{schema_column(1, 101), schema_column(2, 102)};
    const std::array lineage{schema_column(1, 201)};
    const std::array<std::uint32_t, 2> required_boundary{2, 1};
    const std::array<std::uint32_t, 1> required_lineage{1};

    bioseq_cache_path_domain domain{
        .struct_size = sizeof(bioseq_cache_path_domain),
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
        .provider_security_digest_format_revision =
            bioseq_cache_provider_security_digest_format_canonical,
        .provider_security_flags = 0,
    };
    REQUIRE(validate_path_domain(7, &domain).has_value());
    REQUIRE(provider_security_digest(domain) == k_provider_independent_security_digest);

    domain.struct_size =
        offsetof(bioseq_cache_path_domain, provider_security_digest_format_revision);
    REQUIRE(validate_path_domain(7, &domain).error() == provider_contract_error::struct_size);
    domain.struct_size = sizeof(bioseq_cache_path_domain);

    bioseq_cache_decorated_path decorated{
        .struct_size = sizeof(bioseq_cache_decorated_path),
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
    REQUIRE(validate_decorated_path(&domain, &decorated).error() ==
            provider_contract_error::decorated_boundary_mismatch);

    domain.compatibility_family_id = 0;
    REQUIRE(validate_path_domain(7, &domain).error() ==
            provider_contract_error::invalid_compatibility_family);
    domain.compatibility_family_id = 11;
    domain.provider_security_digest_format_revision = 0;
    REQUIRE(validate_path_domain(7, &domain).error() ==
            provider_contract_error::security_namespace);
    domain.provider_security_digest_format_revision =
        bioseq_cache_provider_security_digest_format_canonical;
    domain.provider_security_flags = 1;
    REQUIRE(validate_path_domain(7, &domain).error() ==
            provider_contract_error::security_namespace);
    domain.provider_security_flags = 0;
    domain.provider_security_digest[31] = 0xa5;
    REQUIRE(validate_path_domain(7, &domain).has_value());
    REQUIRE(provider_security_digest(domain).bytes[31] == 0xa5);
}

TEST_CASE("runtime recipe and digest sink reject malformed or oversized data",
          "[component:cache][layer:unit][feature:provider]") {
    bioseq_cache_plan_digest_recipe recipe{
        .struct_size = sizeof(bioseq_cache_plan_digest_recipe),
        .flags = 0,
        .digest_format_revision = k_provider_recipe_format_revision,
        .reserved = 0,
        .plan_domain_nodes = reinterpret_cast<::List*>(0x1),
    };
    REQUIRE(validate_plan_digest_recipe(&recipe, 1, 10, 2).has_value());
    recipe.digest_format_revision = 99;
    REQUIRE(validate_plan_digest_recipe(&recipe, 1, 10, 2).error() ==
            provider_contract_error::recipe_shape);

    std::array<std::byte, 4> storage{};
    bounded_digest_sink_buffer buffer{.bytes = storage};
    auto sink = make_digest_sink(buffer);
    const std::array<std::byte, 3> first{std::byte{1}, std::byte{2}, std::byte{3}};
    REQUIRE(sink.append(sink.opaque, first.data(), first.size()));
    REQUIRE_FALSE(sink.append(sink.opaque, first.data(), first.size()));
    REQUIRE(buffer.failed);
}

TEST_CASE("finalized recipe and expected digest are accepted atomically",
          "[component:cache][layer:unit][feature:provider]") {
    cache_digest_256 digest_a{};
    digest_a.bytes[0] = 0xa1;
    cache_digest_256 digest_b{};
    digest_b.bytes[0] = 0xb2;

    bioseq_cache_plan_digest_recipe recipe{
        .struct_size = sizeof(bioseq_cache_plan_digest_recipe),
        .flags = 0,
        .digest_format_revision = k_provider_recipe_format_revision,
        .reserved = 0,
        .plan_domain_nodes = reinterpret_cast<::List*>(0x1),
    };
    std::copy(digest_a.bytes.begin(), digest_a.bytes.end(), recipe.expected_generation_digest);

    const auto finalized_a = finalize_plan_digest_recipe(&recipe, 1, 37, 4, &digest_a);
    REQUIRE(finalized_a.has_value());
    REQUIRE(finalized_a->expected_generation_digest == digest_a);
    REQUIRE(finalized_a->digest_format_revision == k_provider_recipe_format_revision);
    REQUIRE(finalized_a->recipe_count == 1);
    REQUIRE(finalized_a->serialized_bytes == 37);
    REQUIRE(finalized_a->node_count == 4);

    std::copy(digest_b.bytes.begin(), digest_b.bytes.end(), recipe.expected_generation_digest);
    recipe.plan_domain_nodes = nullptr;
    const auto partial_output = finalize_plan_digest_recipe(&recipe, 1, 37, 4, &digest_a);
    REQUIRE_FALSE(partial_output.has_value());
    REQUIRE(partial_output.error() == provider_contract_error::recipe_shape);
    REQUIRE(finalized_a->expected_generation_digest == digest_a);

    recipe.plan_domain_nodes = reinterpret_cast<::List*>(0x1);
    const auto path_final_mismatch = finalize_plan_digest_recipe(&recipe, 1, 37, 4, &digest_a);
    REQUIRE_FALSE(path_final_mismatch.has_value());
    REQUIRE(path_final_mismatch.error() == provider_contract_error::coverage_digest_mismatch);
    REQUIRE(finalized_a->expected_generation_digest == digest_a);

    const auto exact_b = finalize_plan_digest_recipe(&recipe, 1, 37, 4);
    REQUIRE(exact_b.has_value());
    REQUIRE(exact_b->expected_generation_digest == digest_b);
}

TEST_CASE("branch ownership is occurrence-disjoint and SQL NULL correct",
          "[component:cache][layer:unit][feature:provider]") {
    const std::array<bool, 3> valid{true, false, true};
    const std::array predicates{sql_bool::unknown, sql_bool::true_value, sql_bool::true_value};
    const auto selected = choose_coverage_branch(sql_bool::true_value, predicates, valid);
    REQUIRE(selected.kind == coverage_branch_kind::cached);
    REQUIRE(selected.source_index == 2);
    REQUIRE(source_owns_occurrence(2, sql_bool::true_value, predicates, valid));
    REQUIRE_FALSE(source_owns_occurrence(0, sql_bool::true_value, predicates, valid));

    const std::array no_match{sql_bool::unknown, sql_bool::false_value, sql_bool::unknown};
    REQUIRE(choose_coverage_branch(sql_bool::true_value, no_match, valid).kind ==
            coverage_branch_kind::residual);
    REQUIRE(choose_coverage_branch(sql_bool::unknown, predicates, valid).kind ==
            coverage_branch_kind::drop);
}

TEST_CASE("late fetch sink enforces one value and one terminal per reference",
          "[component:cache][layer:unit][feature:provider]") {
    std::array<std::uint8_t, 2> terminal{};
    std::array<std::uint8_t, 4> seen{};
    std::array<std::uintptr_t, 4> values{};
    std::array<std::uint8_t, 4> nulls{};
    late_fetch_sink_state state{
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
        REQUIRE(sink.emit_value(sink.opaque, reference, 0, 100 + reference, false) ==
                bioseq_cache_status_ok);
        REQUIRE(sink.emit_value(sink.opaque, reference, 1, 0, true) == bioseq_cache_status_ok);
        sink.emit_reference_status(sink.opaque, reference, bioseq_cache_status_ok);
    }
    REQUIRE(late_fetch_complete_and_valid(state));

    REQUIRE(sink.emit_value(sink.opaque, 0, 0, 1, false) == bioseq_cache_status_error);
    REQUIRE_FALSE(late_fetch_complete_and_valid(state));
}

TEST_CASE("candidate cleanup exhausts reference kinds and reentrancy",
          "[component:cache][layer:unit][feature:provider]") {
    release_counter counter{};
    cache_entry_id entry{};
    entry.generation = 9;
    entry.claim_sequence = 13;
    constexpr std::array kinds{
        candidate_reference_kind::provisional_pin,
        candidate_reference_kind::in_flight_waiter,
        candidate_reference_kind::admission_reservation,
    };
    for (const candidate_reference_kind kind : kinds) {
        planner_candidate_cleanup_record next{};
        planner_candidate_cleanup_record cleanup{.next = &next};
        release_candidate_reference_noerror(cleanup);
        REQUIRE_FALSE(activate_candidate_reference(
            cleanup, entry, candidate_reference_kind::none, &count_release, &counter));
        REQUIRE_FALSE(activate_candidate_reference(cleanup, entry, kind, nullptr, &counter));
        REQUIRE_FALSE(cleanup.active);
        REQUIRE(cleanup.next == &next);
        REQUIRE(cleanup.reference_kind == candidate_reference_kind::none);
        REQUIRE(activate_candidate_reference(cleanup, entry, kind, &count_release, &counter));
        REQUIRE_FALSE(activate_candidate_reference(cleanup, entry, kind, &count_release, &counter));
        release_candidate_reference_noerror(cleanup);
        release_candidate_reference_noerror(cleanup);
        REQUIRE_FALSE(cleanup.active);
        REQUIRE(cleanup.reference_kind == candidate_reference_kind::none);
        REQUIRE(cleanup.release == nullptr);
        REQUIRE(cleanup.release_opaque == nullptr);
        REQUIRE(cleanup.next == &next);
        REQUIRE(counter.kind == kind);
    }
    REQUIRE(counter.calls == kinds.size());
    REQUIRE(counter.last.generation == 9);
    REQUIRE(counter.last.claim_sequence == 13);

    planner_candidate_cleanup_record reentrant{};
    reactivating_release_counter reactivating{
        .record = &reentrant,
        .replacement =
            {
                .generation = 10,
                .claim_sequence = 14,
            },
    };
    REQUIRE(activate_candidate_reference(reentrant,
                                         entry,
                                         candidate_reference_kind::provisional_pin,
                                         &reactivate_once_release,
                                         &reactivating));
    release_candidate_reference_noerror(reentrant);
    REQUIRE(reactivating.reactivation_succeeded);
    REQUIRE(reentrant.active);
    REQUIRE(reentrant.reference_kind == candidate_reference_kind::in_flight_waiter);
    release_candidate_reference_noerror(reentrant);
    REQUIRE_FALSE(reentrant.active);
    REQUIRE(reactivating.calls == 2);
    REQUIRE(reactivating.kinds[0] == candidate_reference_kind::provisional_pin);
    REQUIRE(reactivating.kinds[1] == candidate_reference_kind::in_flight_waiter);
}

TEST_CASE("gate publication is one-way and exhaustive at its bounds",
          "[component:cache][layer:unit][feature:provider]") {
    reuse_gate_freeze empty{};
    REQUIRE_FALSE(source_gate_is_valid(empty, 0));
    REQUIRE_FALSE(begin_gate_preflight(empty, 0));
    REQUIRE_FALSE(begin_gate_preflight(empty, k_maximum_sources_per_coverage_node + 1));
    REQUIRE(empty.state == reuse_preflight_state::empty);

    reuse_gate_freeze maximum{};
    REQUIRE(begin_gate_preflight(maximum, k_maximum_sources_per_coverage_node));
    REQUIRE(stage_valid_source(maximum, 0));
    REQUIRE(stage_valid_source(maximum, k_maximum_sources_per_coverage_node - 1));
    REQUIRE_FALSE(stage_valid_source(maximum, k_maximum_sources_per_coverage_node));
    REQUIRE_FALSE(source_gate_is_valid(maximum, 0));
    REQUIRE(freeze_source_decisions(maximum));
    REQUIRE(source_gate_is_valid(maximum, 0));
    REQUIRE(source_gate_is_valid(maximum, k_maximum_sources_per_coverage_node - 1));
    REQUIRE_FALSE(source_gate_is_valid(maximum, k_maximum_sources_per_coverage_node));
    REQUIRE_FALSE(freeze_source_decisions(maximum));
    REQUIRE_FALSE(stage_valid_source(maximum, 1));
    REQUIRE_FALSE(begin_gate_preflight(maximum, 1));
    freeze_all_sources_invalid(maximum);
    REQUIRE(source_gate_is_valid(maximum, 0));

    reuse_gate_freeze fail_open{};
    REQUIRE(begin_gate_preflight(fail_open, 3));
    REQUIRE(stage_valid_source(fail_open, 0));
    REQUIRE(stage_valid_source(fail_open, 2));
    freeze_all_sources_invalid(fail_open);
    REQUIRE(fail_open.state == reuse_preflight_state::frozen);
    for (std::size_t index = 0; index < 3; ++index) {
        REQUIRE_FALSE(source_gate_is_valid(fail_open, index));
    }
}

TEST_CASE("replacement plans retain independent frozen digests and gates",
          "[component:cache][layer:unit][feature:provider]") {
    cache_digest_256 digest_a{};
    digest_a.bytes[0] = 0xa1;
    cache_digest_256 digest_b{};
    digest_b.bytes[0] = 0xb2;
    cache_digest_256 live_configuration_digest = digest_a;
    bioseq_cache_plan_digest_recipe recipe{
        .struct_size = sizeof(bioseq_cache_plan_digest_recipe),
        .digest_format_revision = k_provider_recipe_format_revision,
        .plan_domain_nodes = reinterpret_cast<::List*>(0x1),
    };

    std::copy(live_configuration_digest.bytes.begin(),
              live_configuration_digest.bytes.end(),
              recipe.expected_generation_digest);
    const auto plan_a = finalize_plan_digest_recipe(&recipe, 1, 8, 1, &live_configuration_digest);
    REQUIRE(plan_a.has_value());
    const cache_digest_256 paused_a_begin_digest = plan_a->expected_generation_digest;

    reuse_gate_freeze plan_a_gates{};
    REQUIRE(begin_gate_preflight(plan_a_gates, 2));
    REQUIRE(stage_valid_source(plan_a_gates, 0));
    REQUIRE(freeze_source_decisions(plan_a_gates));

    live_configuration_digest = digest_b;
    std::copy(live_configuration_digest.bytes.begin(),
              live_configuration_digest.bytes.end(),
              recipe.expected_generation_digest);
    const auto plan_b = finalize_plan_digest_recipe(&recipe, 1, 8, 1, &live_configuration_digest);
    REQUIRE(plan_b.has_value());
    const cache_digest_256 replacement_b_begin_digest = plan_b->expected_generation_digest;
    reuse_gate_freeze plan_b_gates{};
    REQUIRE(begin_gate_preflight(plan_b_gates, 2));
    REQUIRE(stage_valid_source(plan_b_gates, 1));
    REQUIRE(freeze_source_decisions(plan_b_gates));

    REQUIRE(runtime_digest_matches(plan_a->expected_generation_digest, paused_a_begin_digest));
    REQUIRE(runtime_digest_matches(plan_b->expected_generation_digest, replacement_b_begin_digest));
    REQUIRE_FALSE(
        runtime_digest_matches(plan_a->expected_generation_digest, live_configuration_digest));
    REQUIRE(runtime_digest_matches(plan_b->expected_generation_digest, live_configuration_digest));
    REQUIRE(source_gate_is_valid(plan_a_gates, 0));
    REQUIRE_FALSE(source_gate_is_valid(plan_a_gates, 1));
    REQUIRE_FALSE(source_gate_is_valid(plan_b_gates, 0));
    REQUIRE(source_gate_is_valid(plan_b_gates, 1));

    REQUIRE_FALSE(begin_gate_preflight(plan_a_gates, 2));
    freeze_all_sources_invalid(plan_a_gates);
    REQUIRE(source_gate_is_valid(plan_a_gates, 0));
    REQUIRE_FALSE(source_gate_is_valid(plan_a_gates, 1));
}

TEST_CASE("reuse waits obey descending owned claim order",
          "[component:cache][layer:unit][feature:provider]") {
    const std::array<std::uint64_t, 3> owned{90, 70, 50};
    REQUIRE(claim_order_allows_wait(49, owned));
    REQUIRE_FALSE(claim_order_allows_wait(50, owned));
    REQUIRE_FALSE(claim_order_allows_wait(91, owned));
    REQUIRE_FALSE(claim_order_allows_wait(0, owned));
}

TEST_CASE("fragment binding semantics ignore raw RTI and separate occurrences",
          "[component:cache][layer:unit][feature:provider]") {
    coverage_fragment_binding_signature first{
        .provider_rte_occurrence_ordinal = 0,
        .provider_rte_occurrence_count = 2,
        .logical_relation_oid = 4100,
        .selected_relation_oid = 4101,
        .provider_id = 77,
    };
    first.binding_role_digest.bytes[0] = 0xa5;
    coverage_fragment_binding_signature second = first;
    second.provider_rte_occurrence_ordinal = 1;
    REQUIRE(valid_coverage_fragment_binding(first));
    REQUIRE(valid_coverage_fragment_binding(second));

    const std::array<std::byte, 3> predicate{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
    std::array<std::byte, 256> first_bytes{};
    std::array<std::byte, 256> second_bytes{};
    const auto first_size = write_coverage_binding_semantics(
        coverage_canonical_key_kind::generation, first, predicate, first_bytes);
    const auto second_size = write_coverage_binding_semantics(
        coverage_canonical_key_kind::generation, second, predicate, second_bytes);
    REQUIRE(first_size.has_value());
    REQUIRE(second_size.has_value());
    REQUIRE(*first_size == *second_size);
    REQUIRE_FALSE(
        std::equal(first_bytes.begin(), first_bytes.begin() + *first_size, second_bytes.begin()));

    first.provider_rte_occurrence_ordinal = 2;
    REQUIRE_FALSE(valid_coverage_fragment_binding(first));
}
