/**
 * @file aggregation_test.cpp
 * @brief Check statistics merging, overflow, and changes in the recorded execution environment.
 */
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.unit.pipeline.pipe_support;

using namespace bioseq::tests::pipeline::pipe_support;

TEST_CASE("merge_job and aggregate merge have identical drift semantics",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const auto aggregate_job = [](const pipeline_job_execution_stats& job) {
        pipeline_execution_stats stats{};
        stats.merge_job(job);
        return stats;
    };
    const auto require_same_counts_and_coordinates = [](
            const pipeline_execution_stats& direct,
            const pipeline_execution_stats& grouped) {
        REQUIRE(direct.requested_backend == grouped.requested_backend);
        REQUIRE(direct.actual_backend == grouped.actual_backend);
        REQUIRE(direct.requested_target_index_source
                == grouped.requested_target_index_source);
        REQUIRE(direct.actual_target_index_source
                == grouped.actual_target_index_source);
        REQUIRE(direct.attempted_alignment_jobs
                == grouped.attempted_alignment_jobs);
        REQUIRE(direct.fully_attested_alignment_jobs
                == grouped.fully_attested_alignment_jobs);
        REQUIRE(direct.unattested_alignment_jobs
                == grouped.unattested_alignment_jobs);
        REQUIRE(direct.first_attempted_target_row
                == grouped.first_attempted_target_row);
        REQUIRE(direct.first_attempted_query_ordinal
                == grouped.first_attempted_query_ordinal);
        REQUIRE(direct.first_unattested_target_row
                == grouped.first_unattested_target_row);
        REQUIRE(direct.first_unattested_query_ordinal
                == grouped.first_unattested_query_ordinal);
    };

    SECTION("persisted generation drift") {
        auto first = make_attested_job(
            executed_backend::vulkan_gpu, target_index_source::persisted);
        first.logical_target_row = 41;
        first.query_ordinal = 5;
        auto second = first;
        second.logical_target_row = 42;
        second.query_ordinal = 6;
        second.persisted_generation_id = 18;

        pipeline_execution_stats direct{};
        direct.merge_job(first);
        direct.merge_job(second);
        auto grouped = aggregate_job(first);
        grouped.merge(aggregate_job(second));

        require_same_counts_and_coordinates(direct, grouped);
        REQUIRE(direct.requested_backend == executed_backend::vulkan_gpu);
        REQUIRE(direct.actual_backend == executed_backend::vulkan_gpu);
        REQUIRE(direct.has_mixed_persisted_identity);
        REQUIRE(grouped.has_mixed_persisted_identity);
        REQUIRE(direct.attempted_alignment_jobs == 2);
        REQUIRE(direct.fully_attested_alignment_jobs == 0);
        REQUIRE(direct.unattested_alignment_jobs == 2);
        REQUIRE(direct.first_unattested_target_row == 41);
        REQUIRE(direct.first_unattested_query_ordinal == 5);
    }

    SECTION("environment drift") {
        auto first = make_attested_job(
            executed_backend::simd_cpu, target_index_source::recomputed);
        first.logical_target_row = 51;
        first.query_ordinal = 7;
        auto second = first;
        second.logical_target_row = 52;
        second.query_ordinal = 8;
        second.environment.effective_cpu_workers = 3;

        pipeline_execution_stats direct{};
        direct.merge_job(first);
        direct.merge_job(second);
        auto grouped = aggregate_job(first);
        grouped.merge(aggregate_job(second));

        require_same_counts_and_coordinates(direct, grouped);
        REQUIRE(direct.requested_backend == executed_backend::simd_cpu);
        REQUIRE(direct.actual_backend == executed_backend::simd_cpu);
        REQUIRE(direct.environment.is_mixed_invalid);
        REQUIRE(grouped.environment.is_mixed_invalid);
        REQUIRE(direct.attempted_alignment_jobs == 2);
        REQUIRE(direct.fully_attested_alignment_jobs == 0);
        REQUIRE(direct.unattested_alignment_jobs == 2);
        REQUIRE(direct.first_unattested_target_row == 51);
        REQUIRE(direct.first_unattested_query_ordinal == 7);
    }
}

TEST_CASE("aggregate merge cannot launder absent environments",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const auto missing_environment_operand = [] {
        pipeline_execution_stats stats{};
        stats.requested_backend = executed_backend::simd_cpu;
        stats.actual_backend = executed_backend::simd_cpu;
        stats.requested_simd_chaining_policy =
            simd_chaining_policy_identity::bounded_batch_approximate;
        stats.observed_simd_chaining_policy =
            simd_chaining_policy_identity::bounded_batch_approximate;
        stats.requested_target_index_source = target_index_source::recomputed;
        stats.actual_target_index_source = target_index_source::recomputed;
        stats.attempted_alignment_jobs = 1;
        stats.fully_attested_alignment_jobs = 1;
        stats.first_attempted_target_row = 91;
        stats.first_attempted_query_ordinal = 4;
        return stats;
    };
    const auto valid_operand = [] {
        pipeline_execution_stats stats{};
        stats.merge_job(make_attested_job(
            executed_backend::simd_cpu, target_index_source::recomputed));
        return stats;
    };

    auto missing_left = missing_environment_operand();
    missing_left.merge(valid_operand());
    REQUIRE(missing_left.environment.is_mixed_invalid);
    REQUIRE(missing_left.attempted_alignment_jobs == 2);
    REQUIRE(missing_left.fully_attested_alignment_jobs == 0);
    REQUIRE(missing_left.unattested_alignment_jobs == 2);
    REQUIRE(missing_left.first_unattested_target_row == 91);
    REQUIRE(missing_left.first_unattested_query_ordinal == 4);

    auto missing_right = valid_operand();
    missing_right.merge(missing_environment_operand());
    REQUIRE(missing_right.environment.is_mixed_invalid);
    REQUIRE(missing_right.attempted_alignment_jobs == 2);
    REQUIRE(missing_right.fully_attested_alignment_jobs == 0);
    REQUIRE(missing_right.unattested_alignment_jobs == 2);
    REQUIRE(missing_right.first_unattested_target_row == 0);
    REQUIRE(missing_right.first_unattested_query_ordinal == 0);

    pipeline_execution_stats zero_job_nonempty{};
    zero_job_nonempty.requested_backend = executed_backend::simd_cpu;
    zero_job_nonempty.actual_backend = executed_backend::simd_cpu;
    zero_job_nonempty.requested_simd_chaining_policy =
        simd_chaining_policy_identity::bounded_batch_approximate;
    zero_job_nonempty.observed_simd_chaining_policy =
        simd_chaining_policy_identity::bounded_batch_approximate;
    zero_job_nonempty.requested_target_index_source = target_index_source::recomputed;
    zero_job_nonempty.actual_target_index_source = target_index_source::recomputed;
    zero_job_nonempty.environment = make_cpu_environment();
    pipeline_execution_stats zero_job_aggregate{};
    zero_job_aggregate.merge(zero_job_nonempty);
    REQUIRE(zero_job_aggregate.environment.is_mixed_invalid);
    REQUIRE(zero_job_aggregate.attempted_alignment_jobs == 0);
    REQUIRE(zero_job_aggregate.fully_attested_alignment_jobs == 0);
    REQUIRE(zero_job_aggregate.unattested_alignment_jobs == 0);
}

TEST_CASE("exact-empty stats checks every stage and operational member",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const pipeline_execution_stats empty{};
    REQUIRE(pipeline_execution_stats::is_exact_empty_stats_operand(empty));

    SECTION("stage coverage is observations even with no attempted jobs") {
        auto injected = empty;
        injected.stages.coverage[
            stage_index(pipeline_stage::query_sketch)].eligible_jobs = 1;
        REQUIRE_FALSE(pipeline_execution_stats::is_exact_empty_stats_operand(
            injected));
    }

    SECTION("stage timing is operational observations") {
        auto injected = empty;
        injected.operational.timing[
            stage_index(pipeline_stage::query_sketch)].host_wall_ns = 1;
        REQUIRE_FALSE(pipeline_execution_stats::is_exact_empty_stats_operand(
            injected));
    }

    SECTION("an ordinary operational counter is observations") {
        auto injected = empty;
        injected.operational.target_rows = 1;
        REQUIRE_FALSE(pipeline_execution_stats::is_exact_empty_stats_operand(
            injected));
    }

    SECTION("a zero-valued report-once identity still has a presence bit") {
        auto injected = empty;
        injected.operational.cpu_prepared_key_set_checksum_available = true;
        REQUIRE_FALSE(pipeline_execution_stats::is_exact_empty_stats_operand(
            injected));
    }
}

TEST_CASE("zero-job stage observations cannot launder a complete aggregate",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    auto job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    job.logical_target_row = 71;
    job.query_ordinal = 8;
    pipeline_execution_stats complete{};
    complete.merge_job(job);
    REQUIRE(pipeline_execution_stats::can_retain_full_attestation(complete));

    pipeline_execution_stats injected{};
    auto& query = injected.stages.coverage[
        stage_index(pipeline_stage::query_sketch)];
    query.eligible_jobs = 1;
    query.executed_jobs[engine_index(execution_engine::simd_cpu)] = 1;
    REQUIRE_FALSE(pipeline_execution_stats::is_exact_empty_stats_operand(
        injected));

    const auto require_rejected = [](const pipeline_execution_stats& stats) {
        REQUIRE(stats.requested_backend == executed_backend::simd_cpu);
        REQUIRE(stats.actual_backend == executed_backend::simd_cpu);
        REQUIRE(stats.attempted_alignment_jobs == 1);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == 1);
        REQUIRE(stats.first_unattested_target_row == 71);
        REQUIRE(stats.first_unattested_query_ordinal == 8);
        REQUIRE_FALSE(pipeline_execution_stats::is_aggregate_stage_coverage_valid(
            stats));
        REQUIRE_FALSE(pipeline_execution_stats::can_retain_full_attestation(
            stats));
    };

    auto injected_on_right = complete;
    injected_on_right.merge(injected);
    require_rejected(injected_on_right);

    auto injected_on_left = injected;
    injected_on_left.merge(complete);
    require_rejected(injected_on_left);
}

TEST_CASE("aggregate retention rejects malformed stage matrices",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    auto job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    job.logical_target_row = 81;
    job.query_ordinal = 6;
    pipeline_execution_stats complete{};
    complete.merge_job(job);
    REQUIRE(pipeline_execution_stats::is_aggregate_stage_coverage_valid(
        complete));
    REQUIRE(pipeline_execution_stats::can_retain_full_attestation(complete));

    const auto require_malformed_stage_rejected = [](
            const pipeline_execution_stats& malformed) {
        REQUIRE_FALSE(pipeline_execution_stats::is_aggregate_stage_coverage_valid(
            malformed));
        REQUIRE_FALSE(pipeline_execution_stats::can_retain_full_attestation(
            malformed));
        pipeline_execution_stats merged{};
        merged.merge(malformed);
        REQUIRE(merged.attempted_alignment_jobs == 1);
        REQUIRE(merged.fully_attested_alignment_jobs == 0);
        REQUIRE(merged.unattested_alignment_jobs == 1);
        REQUIRE(merged.first_unattested_target_row == 81);
        REQUIRE(merged.first_unattested_query_ordinal == 6);
    };

    SECTION("required front-end coverage is missing") {
        auto malformed = complete;
        malformed.stages.coverage[
            stage_index(pipeline_stage::query_sketch)] = {};
        require_malformed_stage_rejected(malformed);
    }

    SECTION("stage total exceeds the attempted-job total") {
        auto malformed = complete;
        auto& query = malformed.stages.coverage[
            stage_index(pipeline_stage::query_sketch)];
        query.eligible_jobs = 2;
        query.executed_jobs[engine_index(execution_engine::simd_cpu)] = 2;
        require_malformed_stage_rejected(malformed);
    }

    SECTION("a forbidden engine lane carries work") {
        auto malformed = complete;
        malformed.stages.coverage[
            stage_index(pipeline_stage::query_sketch)]
            .executed_jobs[engine_index(execution_engine::scalar_cpu)] = 1;
        require_malformed_stage_rejected(malformed);
    }

    SECTION("a fallback lane carries work") {
        auto malformed = complete;
        malformed.stages.coverage[
            stage_index(pipeline_stage::query_sketch)].fallback_jobs = 1;
        require_malformed_stage_rejected(malformed);
    }

    SECTION("a mode-forbidden stage carries work") {
        auto malformed = complete;
        auto& upload = malformed.stages.coverage[
            stage_index(pipeline_stage::device_index_upload)];
        upload.eligible_jobs = 1;
        upload.executed_jobs[engine_index(execution_engine::simd_cpu)] = 1;
        require_malformed_stage_rejected(malformed);
    }

    SECTION("a saturated aggregate cannot retain stats") {
        auto malformed = complete;
        malformed.operational.are_stats_saturated = true;
        REQUIRE(pipeline_execution_stats::is_aggregate_stage_coverage_valid(
            malformed));
        REQUIRE_FALSE(pipeline_execution_stats::can_retain_full_attestation(
            malformed));
        pipeline_execution_stats merged{};
        merged.merge(malformed);
        REQUIRE(merged.fully_attested_alignment_jobs == 0);
        REQUIRE(merged.unattested_alignment_jobs == 1);
        REQUIRE(merged.first_unattested_target_row == 81);
        REQUIRE(merged.first_unattested_query_ordinal == 6);
    }
}

TEST_CASE("backend declines and fallbacks always revoke attestation",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    auto job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    job.logical_target_row = 91;
    job.query_ordinal = 7;

    const auto require_revoked = [](const pipeline_execution_stats& stats) {
        REQUIRE(stats.attempted_alignment_jobs == 1);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == 1);
        REQUIRE(stats.first_unattested_target_row == 91);
        REQUIRE(stats.first_unattested_query_ordinal == 7);
        REQUIRE_FALSE(pipeline_execution_stats::can_retain_full_attestation(
            stats));
    };

    for (const bool is_fallback : {false, true}) {
        pipeline_operational_stats observations{};
        if (is_fallback) {
            observations.backend_fallbacks = 1;
        } else {
            observations.backend_declines = 1;
        }

        pipeline_execution_stats operational_after_job{};
        operational_after_job.merge_job(job);
        operational_after_job.merge_operational(observations);
        require_revoked(operational_after_job);

        pipeline_execution_stats operational_before_job{};
        operational_before_job.merge_operational(observations);
        operational_before_job.merge_job(job);
        require_revoked(operational_before_job);

        pipeline_execution_stats operational_only{};
        operational_only.merge_operational(observations);
        pipeline_execution_stats grouped_after_job{};
        grouped_after_job.merge_job(job);
        grouped_after_job.merge(operational_only);
        require_revoked(grouped_after_job);

        auto grouped_before_job = operational_only;
        pipeline_execution_stats job_only{};
        job_only.merge_job(job);
        grouped_before_job.merge(job_only);
        require_revoked(grouped_before_job);
    }
}

TEST_CASE("mixed CPU diagnostics retain environment without becoming stats",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    pipeline_job_execution_stats job{};
    job.requested_backend = executed_backend::mixed_invalid;
    job.actual_backend = executed_backend::mixed_invalid;
    job.requested_target_index_source = target_index_source::recomputed;
    job.actual_target_index_source = target_index_source::recomputed;
    job.environment = make_cpu_environment();

    pipeline_execution_stats node{};
    node.merge_job(job);
    REQUIRE(node.attempted_alignment_jobs == 1);
    REQUIRE(node.fully_attested_alignment_jobs == 0);
    REQUIRE(node.unattested_alignment_jobs == 1);
    REQUIRE(pipeline_execution_stats::is_diagnostic_environment_valid(node));

    pipeline_execution_stats statement{};
    statement.merge(node);
    REQUIRE(statement.actual_backend == executed_backend::mixed_invalid);
    REQUIRE(statement.fully_attested_alignment_jobs == 0);
    REQUIRE(statement.unattested_alignment_jobs == 1);
    REQUIRE_FALSE(statement.environment.is_mixed_invalid);
    REQUIRE(pipeline_execution_stats::is_diagnostic_environment_valid(statement));

    pipeline_execution_stats valid_sibling{};
    auto valid_job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    valid_job.logical_target_row = 12;
    valid_job.query_ordinal = 3;
    valid_sibling.merge_job(valid_job);
    statement.merge(valid_sibling);
    REQUIRE(statement.actual_backend == executed_backend::mixed_invalid);
    REQUIRE(statement.fully_attested_alignment_jobs == 0);
    REQUIRE(statement.unattested_alignment_jobs == 2);
    REQUIRE(pipeline_execution_stats::is_diagnostic_environment_valid(statement));

    auto gpu_work_with_cpu_environment = node;
    gpu_work_with_cpu_environment.operational.gpu_seed_dispatches = 1;
    REQUIRE_FALSE(pipeline_execution_stats::is_diagnostic_environment_valid(
        gpu_work_with_cpu_environment));
    pipeline_execution_stats rejected_statement{};
    rejected_statement.merge(gpu_work_with_cpu_environment);
    REQUIRE(rejected_statement.environment.is_mixed_invalid);
}

TEST_CASE("aggregate merge is associative under environment drift",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const auto aggregate_job = [](const pipeline_job_execution_stats& job) {
        pipeline_execution_stats stats{};
        stats.merge_job(job);
        return stats;
    };

    auto first_job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    first_job.logical_target_row = 10;
    first_job.query_ordinal = 1;
    auto second_job = first_job;
    second_job.logical_target_row = 20;
    second_job.query_ordinal = 2;
    second_job.environment.effective_cpu_workers = 3;
    auto third_job = second_job;
    third_job.logical_target_row = 30;
    third_job.query_ordinal = 3;

    auto left_grouped = aggregate_job(first_job);
    left_grouped.merge(aggregate_job(second_job));
    left_grouped.merge(aggregate_job(third_job));

    auto right_tail = aggregate_job(second_job);
    right_tail.merge(aggregate_job(third_job));
    auto right_grouped = aggregate_job(first_job);
    right_grouped.merge(right_tail);

    const auto require_same_fail_closed_result = [](
            const pipeline_execution_stats& stats) {
        REQUIRE(stats.requested_backend == executed_backend::simd_cpu);
        REQUIRE(stats.actual_backend == executed_backend::simd_cpu);
        REQUIRE(stats.requested_target_index_source
                == target_index_source::recomputed);
        REQUIRE(stats.actual_target_index_source
                == target_index_source::recomputed);
        REQUIRE(stats.environment.is_mixed_invalid);
        REQUIRE(stats.attempted_alignment_jobs == 3);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == 3);
        REQUIRE(stats.first_attempted_target_row == 10);
        REQUIRE(stats.first_attempted_query_ordinal == 1);
        REQUIRE(stats.first_unattested_target_row == 10);
        REQUIRE(stats.first_unattested_query_ordinal == 1);
    };
    require_same_fail_closed_result(left_grouped);
    require_same_fail_closed_result(right_grouped);
    REQUIRE(left_grouped.attempted_alignment_jobs
            == right_grouped.attempted_alignment_jobs);
    REQUIRE(left_grouped.fully_attested_alignment_jobs
            == right_grouped.fully_attested_alignment_jobs);
    REQUIRE(left_grouped.unattested_alignment_jobs
            == right_grouped.unattested_alignment_jobs);
    REQUIRE(left_grouped.first_unattested_target_row
            == right_grouped.first_unattested_target_row);
    REQUIRE(left_grouped.first_unattested_query_ordinal
            == right_grouped.first_unattested_query_ordinal);
}

TEST_CASE("aggregate merge is associative at SQL bigint saturation",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const auto aggregate_job = [](std::uint64_t target_row) {
        auto job = make_attested_job(
            executed_backend::simd_cpu, target_index_source::recomputed);
        job.logical_target_row = target_row;
        pipeline_execution_stats stats{};
        stats.merge_job(job);
        return stats;
    };

    auto near_limit = aggregate_job(10);
    near_limit.attempted_alignment_jobs = k_sql_bigint_value_max;
    near_limit.fully_attested_alignment_jobs = k_sql_bigint_value_max;
    auto second = aggregate_job(20);
    auto third = aggregate_job(30);

    auto left_grouped = near_limit;
    left_grouped.merge(second);
    left_grouped.merge(third);

    auto right_tail = second;
    right_tail.merge(third);
    auto right_grouped = near_limit;
    right_grouped.merge(right_tail);

    const auto require_saturated = [](const pipeline_execution_stats& stats) {
        REQUIRE(stats.attempted_alignment_jobs == k_sql_bigint_value_max);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == k_sql_bigint_value_max);
        REQUIRE(stats.first_unattested_target_row == 10);
        REQUIRE(stats.first_unattested_query_ordinal == 0);
        REQUIRE(stats.operational.are_stats_saturated);
    };
    require_saturated(left_grouped);
    require_saturated(right_grouped);
    REQUIRE(left_grouped.requested_backend == right_grouped.requested_backend);
    REQUIRE(left_grouped.actual_backend == right_grouped.actual_backend);
    REQUIRE(left_grouped.fully_attested_alignment_jobs
            == right_grouped.fully_attested_alignment_jobs);
    REQUIRE(left_grouped.unattested_alignment_jobs
            == right_grouped.unattested_alignment_jobs);
}

TEST_CASE("operational saturation is fail closed independent of ordering",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    auto job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    job.logical_target_row = 61;
    job.query_ordinal = 9;
    const auto saturate_operational = [](pipeline_execution_stats& stats) {
        pipeline_operational_stats at_limit{};
        at_limit.target_rows = k_sql_bigint_value_max;
        pipeline_operational_stats overflow{};
        overflow.target_rows = 1;
        stats.merge_operational(at_limit);
        stats.merge_operational(overflow);
    };

    pipeline_execution_stats operational_after_job{};
    operational_after_job.merge_job(job);
    saturate_operational(operational_after_job);

    pipeline_execution_stats operational_before_job{};
    saturate_operational(operational_before_job);
    operational_before_job.merge_job(job);

    pipeline_execution_stats operational_only{};
    saturate_operational(operational_only);
    pipeline_execution_stats grouped{};
    grouped.merge_job(job);
    grouped.merge(operational_only);

    const auto require_fail_closed = [](const pipeline_execution_stats& stats) {
        REQUIRE(stats.requested_backend == executed_backend::simd_cpu);
        REQUIRE(stats.actual_backend == executed_backend::simd_cpu);
        REQUIRE(stats.attempted_alignment_jobs == 1);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == 1);
        REQUIRE(stats.first_unattested_target_row == 61);
        REQUIRE(stats.first_unattested_query_ordinal == 9);
        REQUIRE(stats.operational.target_rows == k_sql_bigint_value_max);
        REQUIRE(stats.operational.are_stats_saturated);
    };
    require_fail_closed(operational_after_job);
    require_fail_closed(operational_before_job);
    require_fail_closed(grouped);
}

TEST_CASE("cold-index operational telemetry merges and saturates through one authority",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;

    pipeline_operational_stats aggregate{};
    pipeline_operational_stats first{};
    first.cold_index_sidecar_heap_opens = 2;
    first.cold_index_sidecar_index_opens = 2;
    first.cold_index_lock_acquires = 7;
    first.cold_index_lock_releases = 6;
    first.cold_index_payload_bytes_delivered = 4096;
    first.cold_index_payload_bytes_scanned = 6144;
    first.cold_index_payload_rows_scanned = 3;
    first.cold_index_payload_rows_skipped = 1;
    first.cold_index_shared_blks_read = 3;
    first.grouped_soa_reader_capacity_accounting_passes = 5;
    first.grouped_soa_reader_global_position_storage_preparations = 1;
    first.grouped_soa_reader_global_position_storage_allocations = 1;
    first.grouped_soa_reader_retained_position_count = 37;
    first.grouped_soa_reader_retained_position_capacity = 37;
    first.grouped_soa_reader_directory_capacity_bytes = 10;
    first.grouped_soa_reader_query_hash_capacity_bytes = 7;
    first.grouped_soa_reader_query_record_capacity_bytes = 100;
    first.grouped_soa_reader_ordinal_route_capacity_bytes = 20;
    first.grouped_soa_reader_selected_extent_capacity_bytes = 30;
    first.grouped_soa_reader_current_selected_control_capacity_bytes = 11;
    first.grouped_soa_reader_global_position_capacity_bytes = 296;
    first.grouped_soa_reader_control_payload_capacity_bytes = 13;
    first.grouped_soa_reader_owned_capacity_bytes = 487;
    first.grouped_soa_reader_admitted_bytes = 487;
    first.grouped_soa_reader_exact_position_image_checks = 1;
    first.grouped_soa_reader_retained_layout_checks = 1;
    aggregate.merge(first);

    REQUIRE(aggregate.cold_index_sidecar_heap_opens == 2);
    REQUIRE(aggregate.cold_index_sidecar_index_opens == 2);
    REQUIRE(aggregate.cold_index_lock_acquires == 7);
    REQUIRE(aggregate.cold_index_lock_releases == 6);
    REQUIRE(aggregate.cold_index_payload_bytes_delivered == 4096);
    REQUIRE(aggregate.cold_index_payload_bytes_scanned == 6144);
    REQUIRE(aggregate.cold_index_payload_rows_scanned == 3);
    REQUIRE(aggregate.cold_index_payload_rows_skipped == 1);
    REQUIRE(aggregate.cold_index_shared_blks_read == 3);
    REQUIRE(aggregate.grouped_soa_reader_capacity_accounting_passes == 5);
    REQUIRE(aggregate.grouped_soa_reader_global_position_storage_preparations == 1);
    REQUIRE(aggregate.grouped_soa_reader_global_position_storage_allocations == 1);
    REQUIRE(aggregate.grouped_soa_reader_position_append_growth_reservations == 0);
    REQUIRE(aggregate.grouped_soa_reader_retained_position_count == 37);
    REQUIRE(aggregate.grouped_soa_reader_retained_position_capacity == 37);
    REQUIRE(aggregate.grouped_soa_reader_query_hash_capacity_bytes == 7);
    REQUIRE(aggregate.grouped_soa_reader_current_selected_control_capacity_bytes
            == 11);
    REQUIRE(aggregate.grouped_soa_reader_global_position_capacity_bytes == 296);
    REQUIRE(aggregate.grouped_soa_reader_control_payload_capacity_bytes == 13);
    REQUIRE(aggregate.grouped_soa_reader_owned_capacity_bytes == 487);
    REQUIRE(aggregate.grouped_soa_reader_admitted_bytes == 487);
    REQUIRE(aggregate.grouped_soa_reader_exact_position_image_checks == 1);
    REQUIRE(aggregate.grouped_soa_reader_exact_position_image_failures == 0);
    REQUIRE(aggregate.grouped_soa_reader_retained_layout_checks == 1);
    REQUIRE(aggregate.grouped_soa_reader_retained_layout_failures == 0);
    REQUIRE_FALSE(aggregate.are_grouped_soa_reader_memory_stats_saturated);
    REQUIRE_FALSE(aggregate.are_stats_saturated);

    pipeline_operational_stats second{};
    second.grouped_soa_reader_capacity_accounting_passes = 4;
    second.grouped_soa_reader_global_position_storage_preparations = 1;
    second.grouped_soa_reader_global_position_storage_allocations = 1;
    second.grouped_soa_reader_position_append_growth_reservations = 2;
    second.grouped_soa_reader_retained_position_count = 11;
    second.grouped_soa_reader_retained_position_capacity = 11;
    second.grouped_soa_reader_directory_capacity_bytes = 5;
    second.grouped_soa_reader_query_hash_capacity_bytes = 3;
    second.grouped_soa_reader_query_record_capacity_bytes = 40;
    second.grouped_soa_reader_ordinal_route_capacity_bytes = 8;
    second.grouped_soa_reader_selected_extent_capacity_bytes = 12;
    second.grouped_soa_reader_current_selected_control_capacity_bytes = 5;
    second.grouped_soa_reader_global_position_capacity_bytes = 87;
    second.grouped_soa_reader_control_payload_capacity_bytes = 7;
    second.grouped_soa_reader_owned_capacity_bytes = 167;
    second.grouped_soa_reader_admitted_bytes = 168;
    second.grouped_soa_reader_exact_position_image_checks = 1;
    second.grouped_soa_reader_exact_position_image_failures = 1;
    second.grouped_soa_reader_retained_layout_checks = 1;
    second.grouped_soa_reader_retained_layout_failures = 1;
    second.are_grouped_soa_reader_memory_stats_saturated = true;
    aggregate.merge(second);

    REQUIRE(aggregate.grouped_soa_reader_capacity_accounting_passes == 9);
    REQUIRE(aggregate.grouped_soa_reader_global_position_storage_preparations == 2);
    REQUIRE(aggregate.grouped_soa_reader_global_position_storage_allocations == 2);
    REQUIRE(aggregate.grouped_soa_reader_position_append_growth_reservations == 2);
    REQUIRE(aggregate.grouped_soa_reader_retained_position_count == 48);
    REQUIRE(aggregate.grouped_soa_reader_retained_position_capacity == 48);
    REQUIRE(aggregate.grouped_soa_reader_query_hash_capacity_bytes == 10);
    REQUIRE(aggregate.grouped_soa_reader_current_selected_control_capacity_bytes
            == 16);
    REQUIRE(aggregate.grouped_soa_reader_global_position_capacity_bytes == 383);
    REQUIRE(aggregate.grouped_soa_reader_control_payload_capacity_bytes == 20);
    REQUIRE(aggregate.grouped_soa_reader_owned_capacity_bytes == 654);
    REQUIRE(aggregate.grouped_soa_reader_admitted_bytes == 655);
    REQUIRE(aggregate.grouped_soa_reader_exact_position_image_checks == 2);
    REQUIRE(aggregate.grouped_soa_reader_exact_position_image_failures == 1);
    REQUIRE(aggregate.grouped_soa_reader_retained_layout_checks == 2);
    REQUIRE(aggregate.grouped_soa_reader_retained_layout_failures == 1);
    REQUIRE(aggregate.are_grouped_soa_reader_memory_stats_saturated);
    REQUIRE_FALSE(aggregate.are_stats_saturated);

    pipeline_operational_stats at_limit{};
    at_limit.cold_index_payload_bytes_scanned = k_sql_bigint_value_max;
    at_limit.grouped_soa_reader_retained_position_capacity = k_sql_bigint_value_max;
    pipeline_operational_stats overflow{};
    overflow.cold_index_payload_bytes_scanned = 1;
    overflow.grouped_soa_reader_retained_position_capacity = 1;
    aggregate.merge(at_limit);
    aggregate.merge(overflow);

    REQUIRE(aggregate.cold_index_payload_bytes_scanned
            == k_sql_bigint_value_max);
    REQUIRE(aggregate.grouped_soa_reader_retained_position_capacity
            == k_sql_bigint_value_max);
    REQUIRE(aggregate.are_stats_saturated);

    using capacity_member_type =
        std::uint64_t pipeline_operational_stats::*;
    const auto require_independent_saturation = [](capacity_member_type member) {
        pipeline_operational_stats exact_limit{};
        exact_limit.*member = k_sql_bigint_value_max;
        pipeline_operational_stats independently_merged{};
        independently_merged.merge(exact_limit);
        REQUIRE((independently_merged.*member) == k_sql_bigint_value_max);
        REQUIRE_FALSE(independently_merged.are_stats_saturated);

        pipeline_operational_stats plus_one{};
        plus_one.*member = 1;
        independently_merged.merge(plus_one);
        REQUIRE((independently_merged.*member) == k_sql_bigint_value_max);
        REQUIRE(independently_merged.are_stats_saturated);
    };
    require_independent_saturation(
        &pipeline_operational_stats::grouped_soa_reader_query_hash_capacity_bytes);
    require_independent_saturation(
        &pipeline_operational_stats::
            grouped_soa_reader_current_selected_control_capacity_bytes);
    require_independent_saturation(
        &pipeline_operational_stats::grouped_soa_reader_control_payload_capacity_bytes);
}

TEST_CASE("fragment-native query observations preserves additive and peak merge semantics",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using bioseq::indexing::prepared_fragment_anchor_batch_stats;
    using bioseq::pipeline_stats::pipeline_operational_stats;

    prepared_fragment_anchor_batch_stats first{};
    first.waves = 1u;
    first.queries = 3u;
    first.query_occurrences = 7u;
    first.unique_query_plans = 3u;
    first.packed_occurrence_waves = 1u;
    first.wide_occurrence_waves = 0u;
    first.occurrence_fill_ns = 11u;
    first.control_count_bytes = 101u;
    first.pass2_position_bytes = 40u;
    first.pass3_position_bytes = 40u;
    first.anchors_constructed = 12u;
    first.exact_final_bytes = 288u;
    first.peak_exact_final_bytes = 288u;
    first.global_sort_calls_skipped = 3u;
    first.peak_occurrence_capacity_bytes = 96u;
    first.peak_block_layout_capacity_bytes = 144u;
    first.occurrence_allocations = 1u;

    prepared_fragment_anchor_batch_stats second{};
    second.waves = 2u;
    second.queries = 5u;
    second.query_occurrences = 11u;
    second.unique_query_plans = 4u;
    second.packed_occurrence_waves = 1u;
    second.wide_occurrence_waves = 1u;
    second.occurrence_fill_ns = 17u;
    second.control_count_bytes = 203u;
    second.pass2_position_bytes = 72u;
    second.pass3_position_bytes = 72u;
    second.anchors_constructed = 21u;
    second.exact_final_bytes = 504u;
    second.peak_exact_final_bytes = 360u;
    second.global_sort_calls_skipped = 5u;
    second.peak_occurrence_capacity_bytes = 80u;
    second.peak_block_layout_capacity_bytes = 192u;
    second.occurrence_allocations = 0u;

    first.merge(second);
    REQUIRE(first.waves == 3u);
    REQUIRE(first.queries == 8u);
    REQUIRE(first.query_occurrences == 18u);
    REQUIRE(first.unique_query_plans == 7u);
    REQUIRE(first.query_occurrences >= first.unique_query_plans);
    REQUIRE(first.packed_occurrence_waves == 2u);
    REQUIRE(first.wide_occurrence_waves == 1u);
    REQUIRE(first.packed_occurrence_waves + first.wide_occurrence_waves
        == first.waves);
    REQUIRE(first.occurrence_fill_ns == 28u);
    REQUIRE(first.control_count_bytes == 304u);
    REQUIRE(first.pass2_position_bytes == 112u);
    REQUIRE(first.pass2_position_bytes == first.pass3_position_bytes);
    REQUIRE(first.anchors_constructed == 33u);
    REQUIRE(first.exact_final_bytes == 792u);
    REQUIRE(first.peak_exact_final_bytes == 360u);
    REQUIRE(first.global_sort_calls_invoked == 0u);
    REQUIRE(first.global_sort_calls_skipped == 8u);
    REQUIRE(first.peak_occurrence_capacity_bytes == 96u);
    REQUIRE(first.peak_block_layout_capacity_bytes == 192u);
    REQUIRE(first.occurrence_allocations == 1u);
    REQUIRE(first.forbidden_buffers_are_zero());
    REQUIRE_FALSE(first.saturated);

    pipeline_operational_stats aggregate{};
    pipeline_operational_stats delta{};
    delta.fragment_native_query_stats = first;
    aggregate.merge(delta);
    REQUIRE(aggregate.fragment_native_query_stats == first);
    REQUIRE_FALSE(aggregate.are_stats_saturated);

    pipeline_operational_stats overflow{};
    overflow.fragment_native_query_stats.waves =
        (std::numeric_limits<std::uint64_t>::max)();
    overflow.fragment_native_query_stats.query_occurrences =
        (std::numeric_limits<std::uint64_t>::max)();
    aggregate.merge(overflow);
    REQUIRE(aggregate.fragment_native_query_stats.waves
        == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(aggregate.fragment_native_query_stats.query_occurrences
        == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(aggregate.fragment_native_query_stats.saturated);
    REQUIRE(aggregate.are_stats_saturated);
}

TEST_CASE("grouped-SoA provider observations preserves stable codes and checked counters",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;

    pipeline_operational_stats first{};
    auto& first_stats = first.grouped_soa_provider;
    first_stats.record_provider_configured(1u);
    first_stats.record_provider_captured(1u);
    first_stats.record_provider_persisted(1u);
    first_stats.record_provider_opened(1u);
    first_stats.record_mapped_sink_preallocation_result(
        grouped_soa_operation_result::succeeded);
    first_stats.record_lazy_state_layout(
        grouped_soa_state_layout::compact_aligned_planes);
    first_stats.record_madvise(
        grouped_soa_mapping_advice::random,
        grouped_soa_operation_result::succeeded);
    first_stats.record_counter(first_stats.provider_head_scans, 1u);
    first_stats.record_counter(first_stats.provider_head_rows, 1u);
    first_stats.record_counter(first_stats.mapped_sink_calls, 1u);
    first_stats.record_counter(first_stats.mapped_sink_bytes, 100u);
    first_stats.record_counter(first_stats.mapping_bytes, 400u);
    first_stats.record_file_action_high_water(2u);
    REQUIRE(first_stats.provider_identity_complete_and_consistent());

    pipeline_operational_stats second{};
    auto& second_stats = second.grouped_soa_provider;
    second_stats.record_provider_configured(1u);
    second_stats.record_provider_captured(1u);
    second_stats.record_provider_persisted(1u);
    second_stats.record_provider_opened(1u);
    second_stats.record_mapped_sink_preallocation_result(
        grouped_soa_operation_result::succeeded);
    second_stats.record_lazy_state_layout(
        grouped_soa_state_layout::compact_aligned_planes);
    second_stats.record_madvise(
        grouped_soa_mapping_advice::random,
        grouped_soa_operation_result::succeeded);
    second_stats.record_counter(second_stats.provider_head_scans, 1u);
    second_stats.record_counter(second_stats.provider_head_rows, 1u);
    second_stats.record_counter(second_stats.mapped_sink_calls, 1u);
    second_stats.record_counter(second_stats.mapped_sink_bytes, 200u);
    second_stats.record_counter(second_stats.mapping_bytes, 800u);
    second_stats.record_file_action_high_water(3u);

    pipeline_operational_stats aggregate{};
    aggregate.merge(first);
    aggregate.merge(second);
    const auto& observations = aggregate.grouped_soa_provider;
    REQUIRE(observations.provider_identity_complete_and_consistent());
    REQUIRE(observations.provider_configured == 1u);
    REQUIRE(observations.provider_captured == 1u);
    REQUIRE(observations.provider_persisted == 1u);
    REQUIRE(observations.provider_opened == 1u);
    REQUIRE(observations.provider_head_scans == 2u);
    REQUIRE(observations.provider_head_rows == 2u);
    REQUIRE(observations.mapped_sink_calls == 2u);
    REQUIRE(observations.mapped_sink_bytes == 300u);
    REQUIRE(observations.mapping_bytes == 1'200u);
    REQUIRE(observations.file_action_high_water == 3u);
    REQUIRE(observations.mapped_sink_preallocation_result
        == static_cast<std::uint64_t>(
            grouped_soa_operation_result::succeeded));
    REQUIRE(observations.lazy_state_layout
        == static_cast<std::uint64_t>(
            grouped_soa_state_layout::compact_aligned_planes));
    REQUIRE(observations.madvise_choice
        == static_cast<std::uint64_t>(grouped_soa_mapping_advice::random));
    REQUIRE(observations.madvise_result
        == static_cast<std::uint64_t>(
            grouped_soa_operation_result::succeeded));
    REQUIRE_FALSE(observations.has_stable_value_conflict);
    REQUIRE_FALSE(observations.is_saturated);
    REQUIRE_FALSE(aggregate.has_operational_identity_conflict);
    REQUIRE_FALSE(aggregate.are_grouped_soa_reader_memory_stats_saturated);
    REQUIRE_FALSE(aggregate.are_stats_saturated);

    pipeline_operational_stats conflicting{};
    conflicting.grouped_soa_provider.record_provider_opened(0u);
    aggregate.merge(conflicting);
    REQUIRE(aggregate.has_operational_identity_conflict);
    REQUIRE(aggregate.grouped_soa_provider.has_stable_value_conflict);
    REQUIRE_FALSE(
        aggregate.grouped_soa_provider.provider_identity_complete_and_consistent());

    pipeline_operational_stats exact_limit{};
    exact_limit.grouped_soa_provider.record_counter(
        exact_limit.grouped_soa_provider.mapping_bytes,
        k_sql_bigint_value_max);
    pipeline_operational_stats saturated_aggregate{};
    saturated_aggregate.merge(exact_limit);
    REQUIRE(saturated_aggregate.grouped_soa_provider.mapping_bytes
        == k_sql_bigint_value_max);
    REQUIRE_FALSE(saturated_aggregate.are_stats_saturated);

    pipeline_operational_stats plus_one{};
    plus_one.grouped_soa_provider.record_counter(
        plus_one.grouped_soa_provider.mapping_bytes, 1u);
    saturated_aggregate.merge(plus_one);
    REQUIRE(saturated_aggregate.grouped_soa_provider.mapping_bytes
        == k_sql_bigint_value_max);
    REQUIRE(saturated_aggregate.grouped_soa_provider.is_saturated);
    REQUIRE(saturated_aggregate.are_grouped_soa_reader_memory_stats_saturated);
    REQUIRE(saturated_aggregate.are_stats_saturated);
}

TEST_CASE("hash-major reader receipts preserve max peaks and owners emit once",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;

    pipeline_operational_stats first{};
    auto& first_hash_major = first.hash_major_index_reader;
    first_hash_major.layout_revision = 6;
    first_hash_major.sessions = 1;
    first_hash_major.identity_available = true;
    first_hash_major.generation_id = 71;
    first_hash_major.source_identity[0] = std::byte{0x11};
    first_hash_major.source_content_crc64 = 91;
    first_hash_major.storage_contract_digest[0] = std::byte{0x22};
    first_hash_major.directory_rows = 2;
    first_hash_major.posting_ranges_delivered = 3;
    first_hash_major.posting_range_crc_receipts = 1;
    first_hash_major.posting_semantic_receipts = 3;
    first_hash_major.directory_peak_bytes = 100;
    first_hash_major.control_peak_bytes = 300;
    first_hash_major.posting_peak_bytes = 200;
    first_hash_major.peak_admitted_bytes = 300;
    first_hash_major.owner_bytes_available = true;
    first_hash_major.retained_owner_bytes = 80;
    first_hash_major.admitted_bytes = 80;

    pipeline_operational_stats second = first;
    auto& second_hash_major = second.hash_major_index_reader;
    second_hash_major.directory_rows = 4;
    second_hash_major.posting_ranges_delivered = 5;
    second_hash_major.posting_range_crc_receipts = 2;
    second_hash_major.posting_semantic_receipts = 5;
    second_hash_major.directory_peak_bytes = 500;
    second_hash_major.control_peak_bytes = 250;
    second_hash_major.posting_peak_bytes = 450;
    second_hash_major.peak_admitted_bytes = 500;

    pipeline_operational_stats aggregate{};
    aggregate.merge(first);
    aggregate.merge(second);
    const auto& merged = aggregate.hash_major_index_reader;
    REQUIRE(merged.sessions == 2);
    REQUIRE(merged.directory_rows == 6);
    REQUIRE(merged.posting_ranges_delivered == 8);
    REQUIRE(merged.posting_range_crc_receipts == 3);
    REQUIRE(merged.posting_semantic_receipts == 8);
    REQUIRE(merged.directory_peak_bytes == 500);
    REQUIRE(merged.control_peak_bytes == 300);
    REQUIRE(merged.posting_peak_bytes == 450);
    REQUIRE(merged.peak_admitted_bytes == 500);
    REQUIRE(merged.retained_owner_bytes == 80);
    REQUIRE(merged.admitted_bytes == 80);
    REQUIRE_FALSE(aggregate.has_operational_identity_conflict);
    REQUIRE_FALSE(aggregate.are_stats_saturated);

    pipeline_operational_stats mismatched_owner = first;
    mismatched_owner.hash_major_index_reader.retained_owner_bytes = 81;
    aggregate.merge(mismatched_owner);
    REQUIRE(aggregate.has_operational_identity_conflict);

    pipeline_operational_stats at_limit{};
    at_limit.hash_major_index_reader.directory_rows =
        k_sql_bigint_value_max;
    pipeline_operational_stats overflow{};
    overflow.hash_major_index_reader.directory_rows = 1;
    at_limit.merge(overflow);
    REQUIRE(
        at_limit.hash_major_index_reader.directory_rows
        == k_sql_bigint_value_max);
    REQUIRE(at_limit.hash_major_index_reader.is_saturated);
    REQUIRE(at_limit.are_stats_saturated);
}

TEST_CASE("aggregate environment drift reclassifies both job sets",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const auto aggregate_job = [](const pipeline_job_execution_stats& job) {
        pipeline_execution_stats stats{};
        stats.merge_job(job);
        return stats;
    };
    const auto require_reclassified = [](const pipeline_execution_stats& stats) {
        REQUIRE(stats.environment.is_mixed_invalid);
        REQUIRE(stats.attempted_alignment_jobs == 2);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == 2);
    };

    auto worker_left_job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    auto worker_right_job = worker_left_job;
    worker_right_job.environment.effective_cpu_workers = 3;
    auto worker_left = aggregate_job(worker_left_job);
    worker_left.merge(aggregate_job(worker_right_job));
    require_reclassified(worker_left);

    auto device_left_job = make_attested_job(
        executed_backend::vulkan_gpu, target_index_source::persisted);
    auto device_right_job = device_left_job;
    device_right_job.environment.gpu_device_uuid[1] = 0x7f;
    auto device_left = aggregate_job(device_left_job);
    device_left.merge(aggregate_job(device_right_job));
    require_reclassified(device_left);

    auto cache_right_job = device_left_job;
    cache_right_job.environment.gpu_pipeline_cache_seed_sha256[1] = 0x55;
    auto cache_left = aggregate_job(device_left_job);
    cache_left.merge(aggregate_job(cache_right_job));
    require_reclassified(cache_left);
}

TEST_CASE("per-job coverage does not multiply operational events",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    pipeline_execution_stats aggregate{};
    auto job = make_attested_job(
        executed_backend::vulkan_gpu, target_index_source::persisted);
    for (std::uint64_t i = 0; i < 4096; ++i) {
        job.logical_target_row = i;
        job.query_ordinal = i;
        aggregate.merge_job(job);
    }
    REQUIRE(aggregate.operational.target_rows == 0);

    pipeline_operational_stats once{};
    once.target_rows = 1;
    once.gpu_queue_submits = 1;
    once.gpu_persisted_resident_uploads = 1;
    aggregate.merge_operational(once);

    REQUIRE(aggregate.attempted_alignment_jobs == 4096);
    REQUIRE(aggregate.fully_attested_alignment_jobs == 4096);
    REQUIRE(aggregate.unattested_alignment_jobs == 0);
    REQUIRE(aggregate.first_unattested_target_row == k_unset_job_ordinal);
    REQUIRE(aggregate.stages.coverage[stage_index(pipeline_stage::query_sketch)].eligible_jobs
            == 4096);
    REQUIRE(aggregate.stages.coverage[stage_index(pipeline_stage::query_sketch)]
                .executed_jobs[engine_index(execution_engine::vulkan_gpu)]
            == 4096);
    REQUIRE(aggregate.operational.gpu_queue_submits == 1);
    REQUIRE(aggregate.operational.gpu_persisted_resident_uploads == 1);
    REQUIRE(aggregate.operational.target_rows == 1);
    REQUIRE_FALSE(aggregate.operational.are_stats_saturated);
}

TEST_CASE("dispatch primary and extra-query aggregates merge once",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const auto incomplete_invocation = [](
            const std::uint64_t query_ordinal,
            const std::uint64_t seed_dispatches,
            const bool primary_invocation) {
        pipeline_execution_stats invocation{};
        pipeline_job_execution_stats job{};
        job.requested_backend = executed_backend::vulkan_gpu;
        job.actual_backend = executed_backend::mixed_invalid;
        job.requested_target_index_source = target_index_source::recomputed;
        job.actual_target_index_source = target_index_source::recomputed;
        job.logical_target_row = 41;
        job.query_ordinal = query_ordinal;
        invocation.merge_job(job);
        if (primary_invocation) {
            invocation.operational.target_rows = 1;
            invocation.operational.recomputed_target_index_rows = 1;
        }
        invocation.operational.gpu_seed_dispatches = seed_dispatches;
        return invocation;
    };

    pipeline_execution_stats completed_buffer{};
    completed_buffer.merge(incomplete_invocation(0, 1, true));
    completed_buffer.merge(incomplete_invocation(1, 2, false));

    REQUIRE(completed_buffer.requested_backend == executed_backend::vulkan_gpu);
    REQUIRE(completed_buffer.actual_backend == executed_backend::mixed_invalid);
    REQUIRE(completed_buffer.requested_target_index_source
            == target_index_source::recomputed);
    REQUIRE(completed_buffer.actual_target_index_source
            == target_index_source::recomputed);
    REQUIRE(completed_buffer.attempted_alignment_jobs == 2);
    REQUIRE(completed_buffer.fully_attested_alignment_jobs == 0);
    REQUIRE(completed_buffer.unattested_alignment_jobs == 2);
    REQUIRE(completed_buffer.first_unattested_target_row == 41);
    REQUIRE(completed_buffer.first_unattested_query_ordinal == 0);
    REQUIRE(completed_buffer.operational.target_rows == 1);
    REQUIRE(completed_buffer.operational.persisted_index_rows == 0);
    REQUIRE(completed_buffer.operational.recomputed_target_index_rows == 1);
    REQUIRE(completed_buffer.operational.gpu_seed_dispatches == 3);
    REQUIRE(completed_buffer.environment.is_mixed_invalid);
    REQUIRE_FALSE(completed_buffer.operational.are_stats_saturated);
}

TEST_CASE("persisted request cannot be laundered by recompute outcome",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    pipeline_execution_stats invocation{};
    pipeline_job_execution_stats job{};
    job.requested_backend = executed_backend::simd_cpu;
    job.actual_backend = executed_backend::mixed_invalid;
    job.requested_simd_chaining_policy =
        simd_chaining_policy_identity::bounded_batch_approximate;
    job.observed_simd_chaining_policy = simd_chaining_policy_identity::none;
    job.requested_target_index_source = target_index_source::persisted;
    job.actual_target_index_source = target_index_source::recomputed;
    job.logical_target_row = 19;
    job.query_ordinal = 3;
    invocation.merge_job(job);

    pipeline_operational_stats operational{};
    operational.target_rows = 1;
    operational.missing_persisted_index_rows = 1;
    operational.recomputed_target_index_rows = 1;
    invocation.merge_operational(operational);

    REQUIRE(invocation.requested_target_index_source
            == target_index_source::persisted);
    REQUIRE(invocation.actual_target_index_source
            == target_index_source::recomputed);
    REQUIRE(invocation.attempted_alignment_jobs == 1);
    REQUIRE(invocation.fully_attested_alignment_jobs == 0);
    REQUIRE(invocation.unattested_alignment_jobs == 1);
    REQUIRE(invocation.first_unattested_target_row == 19);
    REQUIRE(invocation.first_unattested_query_ordinal == 3);
    REQUIRE(invocation.operational.target_rows == 1);
    REQUIRE(invocation.operational.persisted_index_rows == 0);
    REQUIRE(invocation.operational.missing_persisted_index_rows == 1);
    REQUIRE(invocation.operational.recomputed_target_index_rows == 1);
    REQUIRE_FALSE(invocation.has_mixed_persisted_identity);
}

TEST_CASE("logical target identity survives payload coalescing",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using bioseq::pipeline_stats::count_unique_logical_target_rows;

    // Distinct table rows count  separately even when their sequence data is
    // identical and shares GPU storage.
    std::array<std::uint64_t, 2> identical_payload_distinct_rows{41, 42};
    REQUIRE(count_unique_logical_target_rows(
                std::span<std::uint64_t>{identical_payload_distinct_rows})
            == 2);

    // Count repeated observations of the same row once.
    std::array<std::uint64_t, 2> repeated_logical_row{41, 41};
    REQUIRE(count_unique_logical_target_rows(
                std::span<std::uint64_t>{repeated_logical_row})
            == 1);
}
