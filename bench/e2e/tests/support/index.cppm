/**
 * @file bench/e2e/tests/support/index.cppm
 * @brief Target-owned contract surface for E2E benchmark tests.
 */
export module bioseq.bench.e2e.tests.support;

import bioseq.bench.e2e;

export namespace bioseq::bench::e2e::tests::support {

using e2e_input = detail::e2e_input;
using e2e_just_index_setup_requirements =
    detail::e2e_just_index_setup_requirements;
using e2e_output = detail::e2e_output;
using e2e_query_oracle_observation =
    detail::e2e_query_oracle_observation;
using e2e_query_truth = detail::e2e_query_truth;
using logical_query_chunk = detail::logical_query_chunk;

using config_value = detail::config_value;
using parameter_map = detail::parameter_map;
using resolved_execution_tuple = detail::resolved_execution_tuple;
using rowset = detail::rowset;
using run_constraints = detail::run_constraints;

using detail::algorithm_matrix_session_sql;
using detail::batched_transformed_coordinate_sql;
using detail::annotate_behavior_differences;
using detail::cold_index_stats_keys;
using detail::evaluate;
using detail::is_canonical_campaign_e2e_config_identity;
using detail::is_complete_vulkan_algorithm_matrix_tuple;
using detail::minimizer_fragment_reader_stats_keys;
using detail::is_sliced_pipeline_stats_configuration_valid;
using detail::make_logical_query_chunks;
using detail::minimizers_per_fragment_index;
using detail::ordinary_algorithm_uses_iteration_boundaries;
using detail::parse_bioseq_rows;
using detail::parse_minimap2_rows;
using detail::persisted_gpu_operation_keys;
using detail::persisted_setup_must_reject;
using detail::projection_sql;
using detail::finalize_paired_oracle_repeat;
using detail::resolve_just_index_setup_requirements;
using detail::serialize_raw_sample;
using detail::sliced_no_index_algorithm;

}  // namespace bioseq::bench::e2e::tests::support
